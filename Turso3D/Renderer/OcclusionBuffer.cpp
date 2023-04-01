#include "../IO/Log.h"
#include "../Math/IntRect.h"
#include "../Thread/WorkQueue.h"
#include "Camera.h"
#include "OcclusionBuffer.h"

#include <cstring>
#include <tracy/Tracy.hpp>

// Rasterizer code based on Chris Hecker's Perspective Texture Mapping series in the Game Developer magazine
// Also available online at http://chrishecker.com/Miscellaneous_Technical_Articles

static const unsigned CLIPMASK_X_POS = 0x1;
static const unsigned CLIPMASK_X_NEG = 0x2;
static const unsigned CLIPMASK_Y_POS = 0x4;
static const unsigned CLIPMASK_Y_NEG = 0x8;
static const unsigned CLIPMASK_Z_POS = 0x10;
static const unsigned CLIPMASK_Z_NEG = 0x20;

OcclusionBuffer::OcclusionBuffer() :
    buffer(nullptr),
    width(0),
    height(0),
    numTriangleBatches(0),
    numReadyMipBuffers(0)
{
    workQueue = Subsystem<WorkQueue>();

    depthHierarchyTask = new MemberFunctionTask<OcclusionBuffer>(this, &OcclusionBuffer::BuildDepthHierarchyWork);
    for (size_t i = 0; i < OCCLUSION_BUFFER_SLICES; ++i)
    {
        rasterizeTrianglesTasks[i] = new RasterizeTrianglesTask(this, &OcclusionBuffer::RasterizeTrianglesWork);
        rasterizeTrianglesTasks[i]->sliceIdx = i;
    }

    numPendingRasterizeTasks.store(0);
}

OcclusionBuffer::~OcclusionBuffer()
{
}

bool OcclusionBuffer::SetSize(int newWidth, int newHeight)
{
    // Force the height to an even amount of pixels for better mip generation
    if (height & 1)
        ++height;
    
    if (newWidth == width && newHeight == height)
        return true;
    
    ZoneScoped;

    if (newWidth <= 0 || newHeight <= 0)
        return false;
    
    if (!IsPowerOfTwo(width))
    {
        LOGERROR("Occlusion buffer width is not a power of two");
        return false;
    }
    
    width = newWidth;
    height = newHeight;
    sliceHeight = (newHeight / OCCLUSION_BUFFER_SLICES) + 1;
    activeSlices = 0;

    for (int i = 0; i < OCCLUSION_BUFFER_SLICES; ++i)
    {
        if (i * sliceHeight < height)
        {
            rasterizeTrianglesTasks[i]->startY = i * sliceHeight;
            rasterizeTrianglesTasks[i]->endY = Min((int)(i + 1) * sliceHeight, height);
            ++activeSlices;
        }
        else
            break;
    }

    // Reserve extra memory in case 3D clipping is not exact
    fullBuffer = new int[width * (height + 2) + 2];
    buffer = fullBuffer + width + 1;
    mipBuffers.clear();
    
    // Build buffers for mip levels
    for (;;)
    {
        newWidth = (newWidth + 1) / 2;
        newHeight = (newHeight + 1) / 2;
        
        mipBuffers.push_back(SharedArrayPtr<DepthValue>(new DepthValue[newWidth * newHeight]));
        
        if (newWidth <= OCCLUSION_MIN_SIZE && newHeight <= OCCLUSION_MIN_SIZE)
            break;
    }
    
    LOGDEBUGF("Set occlusion buffer size %dx%d with %d mip levels", width, height, (int)mipBuffers.size());
    
    CalculateViewport();
    return true;
}

void OcclusionBuffer::SetView(Camera* camera)
{
    if (!camera)
        return;
    
    view = camera->ViewMatrix();
    projection = camera->ProjectionMatrix(false);
    viewProj = projection * view;
    CalculateViewport();
}

void OcclusionBuffer::Reset()
{
    // Make sure to complete previous work before resetting to avoid out of sync state
    Complete();

    numTriangleBatches = 0;
    numReadyMipBuffers = 0;
    numPendingGenerateTasks.store(0);
    numPendingRasterizeTasks.store(0);
}

void OcclusionBuffer::AddTriangles(const Matrix3x4& worldTransform, const void* vertexData, size_t vertexSize, size_t vertexStart, size_t vertexCount)
{
    if (generateTrianglesTasks.size() <= numTriangleBatches)
        generateTrianglesTasks.push_back(new GenerateTrianglesTask(this, &OcclusionBuffer::GenerateTrianglesWork));

    GenerateTrianglesTask* task = generateTrianglesTasks[numTriangleBatches];
    TriangleDrawBatch& batch = task->batch;

    batch.worldTransform = worldTransform;
    batch.vertexData = ((const unsigned char*)vertexData) + vertexStart * vertexSize;
    batch.vertexSize = vertexSize;
    batch.indexData = nullptr;
    batch.drawCount = vertexCount;

    task->triangles.clear();
    for (int i = 0; i < activeSlices; ++i)
        task->triangleIndices[i].clear();

    ++numTriangleBatches;
}

void OcclusionBuffer::AddTriangles(const Matrix3x4& worldTransform, const void* vertexData, size_t vertexSize, const void* indexData, size_t indexSize, size_t indexStart, size_t indexCount)
{
    if (generateTrianglesTasks.size() <= numTriangleBatches)
        generateTrianglesTasks.push_back(new GenerateTrianglesTask(this, &OcclusionBuffer::GenerateTrianglesWork));

    GenerateTrianglesTask* task = generateTrianglesTasks[numTriangleBatches];
    TriangleDrawBatch& batch = task->batch;

    batch.worldTransform = worldTransform;
    batch.vertexData = ((const unsigned char*)vertexData);
    batch.vertexSize = vertexSize;
    batch.indexData = ((const unsigned char*)indexData) + indexSize * indexStart;
    batch.indexSize = indexSize;
    batch.drawCount = indexCount;

    task->triangles.clear();
    for (int i = 0; i < activeSlices; ++i)
        task->triangleIndices[i].clear();

    ++numTriangleBatches;
}

void OcclusionBuffer::DrawTriangles()
{
    // Avoid beginning the work twice
    if (!buffer || !IsCompleted())
        return;

    if (numTriangleBatches)
    {
        numPendingGenerateTasks.store((int)numTriangleBatches);
        numPendingRasterizeTasks.store(1); // Have non-zero counter at this point for correct completion check. It will be loaded with slice count once triangles are ready
        workQueue->QueueTasks(numTriangleBatches, reinterpret_cast<Task**>(&generateTrianglesTasks[0]));
    }
}

void OcclusionBuffer::Complete()
{
    while (numPendingRasterizeTasks.load() > 0)
        workQueue->TryComplete();
}

bool OcclusionBuffer::IsCompleted() const
{
    return !numPendingRasterizeTasks.load();
}

bool OcclusionBuffer::IsVisible(const BoundingBox& worldSpaceBox) const
{
    if (!buffer || !numTriangleBatches)
        return true;
    
    // Transform corners to projection space
    Vector4 vertices[8];
    vertices[0] = ModelTransform(viewProj, worldSpaceBox.min);
    vertices[1] = ModelTransform(viewProj, Vector3(worldSpaceBox.max.x, worldSpaceBox.min.y, worldSpaceBox.min.z));
    vertices[2] = ModelTransform(viewProj, Vector3(worldSpaceBox.min.x, worldSpaceBox.max.y, worldSpaceBox.min.z));
    vertices[3] = ModelTransform(viewProj, Vector3(worldSpaceBox.max.x, worldSpaceBox.max.y, worldSpaceBox.min.z));
    vertices[4] = ModelTransform(viewProj, Vector3(worldSpaceBox.min.x, worldSpaceBox.min.y, worldSpaceBox.max.z));
    vertices[5] = ModelTransform(viewProj, Vector3(worldSpaceBox.max.x, worldSpaceBox.min.y, worldSpaceBox.max.z));
    vertices[6] = ModelTransform(viewProj, Vector3(worldSpaceBox.min.x, worldSpaceBox.max.y, worldSpaceBox.max.z));
    vertices[7] = ModelTransform(viewProj, worldSpaceBox.max);
    
    // Transform to screen space. If any of the corners cross the near plane, assume visible
    float minX, maxX, minY, maxY, minZ;
    
    if (vertices[0].z <= 0.0f)
        return true;
    
    Vector3 projected = ViewportTransform(vertices[0]);
    minX = maxX = projected.x;
    minY = maxY = projected.y;
    minZ = projected.z;
    
    // Project the rest
    for (size_t i = 1; i < 8; ++i)
    {
        if (vertices[i].z <= 0.0f)
            return true;
        
        projected = ViewportTransform(vertices[i]);
        
        if (projected.x < minX) minX = projected.x;
        if (projected.x > maxX) maxX = projected.x;
        if (projected.y < minY) minY = projected.y;
        if (projected.y > maxY) maxY = projected.y;
        if (projected.z < minZ) minZ = projected.z;
    }
    
    // Expand the bounding box 1 pixel in each direction to be conservative and correct rasterization offset
    IntRect rect(
        (int)(minX - 1.5f), (int)(minY - 1.5f),
        (int)(maxX + 0.5f), (int)(maxY + 0.5f)
    );
    
    // Clipping of rect
    if (rect.left < 0)
        rect.left = 0;
    if (rect.top < 0)
        rect.top = 0;
    if (rect.right >= width)
        rect.right = width - 1;
    if (rect.bottom >= height)
        rect.bottom = height - 1;
    
    // Convert depth to integer. Subtract a depth bias that accounts for maximum possible gradient error, 1 depth unit per horizontal pixel
    int z = (int)minZ - width;
    
    // Start from lowest available mip level and check if a conclusive result can be found
    for (int i = (int)numReadyMipBuffers - 1; i >= 0; --i)
    {
        int shift = i + 1;
        int mipWidth = width >> shift;
        int left = rect.left >> shift;
        int right = rect.right >> shift;
        
        DepthValue* mipBuffer = mipBuffers[i];
        DepthValue* row = mipBuffer + (rect.top >> shift) * mipWidth;
        DepthValue* endRow = mipBuffer + (rect.bottom >> shift) * mipWidth;
        bool allOccluded = true;
        
        while (row <= endRow)
        {
            DepthValue* src = row + left;
            DepthValue* end = row + right;
            while (src <= end)
            {
                if (z <= src->min)
                    return true;
                if (z <= src->max)
                    allOccluded = false;
                ++src;
            }
            row += mipWidth;
        }
        
        if (allOccluded)
            return false;
    }
    
    // If no conclusive result, finally check the pixel-level data
    int* row = buffer + rect.top * width;
    int* endRow = buffer + rect.bottom * width;
    while (row <= endRow)
    {
        int* src = row + rect.left;
        int* end = row + rect.right;
        while (src <= end)
        {
            if (z <= *src)
                return true;
            ++src;
        }
        row += width;
    }
    
    return false;
}

void OcclusionBuffer::CalculateViewport()
{
    // Add half pixel offset due to 3D frustum culling
    scaleX = 0.5f * width;
    scaleY = -0.5f * height;
    offsetX = 0.5f * width + 0.5f;
    offsetY = 0.5f * height + 0.5f;
}

void OcclusionBuffer::AddTriangle(GenerateTrianglesTask* task, Vector4* vertices)
{
    unsigned clipMask = 0;
    unsigned andClipMask = 0;
    
    // Build the clip plane mask for the triangle
    for (size_t i = 0; i < 3; ++i)
    {
        unsigned vertexClipMask = 0;
        
        if (vertices[i].x > vertices[i].w)
            vertexClipMask |= CLIPMASK_X_POS;
        if (vertices[i].x < -vertices[i].w)
            vertexClipMask |= CLIPMASK_X_NEG;
        if (vertices[i].y > vertices[i].w)
            vertexClipMask |= CLIPMASK_Y_POS;
        if (vertices[i].y < -vertices[i].w)
            vertexClipMask |= CLIPMASK_Y_NEG;
        if (vertices[i].z > vertices[i].w)
            vertexClipMask |= CLIPMASK_Z_POS;
        if (vertices[i].z < 0.0f)
            vertexClipMask |= CLIPMASK_Z_NEG;
        
        clipMask |= vertexClipMask;
        
        if (!i)
            andClipMask = vertexClipMask;
        else
            andClipMask &= vertexClipMask;
    }
    
    // If triangle is fully behind any clip plane, can reject quickly
    if (andClipMask)
        return;
    
    GradientTriangle projected;

    // Check if triangle is fully inside
    if (!clipMask)
    {
        projected.vertices[0] = ViewportTransform(vertices[0]);
        projected.vertices[1] = ViewportTransform(vertices[1]);
        projected.vertices[2] = ViewportTransform(vertices[2]);
        
        if (CheckFacing(projected.vertices[0], projected.vertices[1], projected.vertices[2]))
        {
            unsigned idx = (unsigned)task->triangles.size();
            int minY = Min(Min((int)projected.vertices[0].y, (int)projected.vertices[1].y), (int)projected.vertices[2].y);
            int maxY = Max(Max((int)projected.vertices[0].y, (int)projected.vertices[1].y), (int)projected.vertices[2].y);

            projected.gradients.Calculate(projected.vertices);
            task->triangles.push_back(projected);

            // Add to needed slices
            for (int i = 0; i < activeSlices; ++i)
            {
                int sliceStartY = i * sliceHeight;
                int sliceEndY = Min(height, sliceStartY + sliceHeight);
                if (minY < sliceEndY && maxY > sliceStartY)
                    task->triangleIndices[i].push_back(idx);
            }
        }
    }
    else
    {
        bool clipTriangles[64];
        
        // Initial triangle
        clipTriangles[0] = true;
        size_t numClipTriangles = 1;
        
        if (clipMask & CLIPMASK_X_POS)
            ClipVertices(Vector4(-1.0f, 0.0f, 0.0f, 1.0f), vertices, clipTriangles, numClipTriangles);
        if (clipMask & CLIPMASK_X_NEG)
            ClipVertices(Vector4(1.0f, 0.0f, 0.0f, 1.0f), vertices, clipTriangles, numClipTriangles);
        if (clipMask & CLIPMASK_Y_POS)
            ClipVertices(Vector4(0.0f, -1.0f, 0.0f, 1.0f), vertices, clipTriangles, numClipTriangles);
        if (clipMask & CLIPMASK_Y_NEG)
            ClipVertices(Vector4(0.0f, 1.0f, 0.0f, 1.0f), vertices, clipTriangles, numClipTriangles);
        if (clipMask & CLIPMASK_Z_POS)
            ClipVertices(Vector4(0.0f, 0.0f, -1.0f, 1.0f), vertices, clipTriangles, numClipTriangles);
        if (clipMask & CLIPMASK_Z_NEG)
            ClipVertices(Vector4(0.0f, 0.0f, 1.0f, 0.0f), vertices, clipTriangles, numClipTriangles);
        
        // Add each accepted triangle
        for (size_t i = 0; i < numClipTriangles; ++i)
        {
            if (clipTriangles[i])
            {
                size_t index = i * 3;
                projected.vertices[0] = ViewportTransform(vertices[index]);
                projected.vertices[1] = ViewportTransform(vertices[index + 1]);
                projected.vertices[2] = ViewportTransform(vertices[index + 2]);

                if (CheckFacing(projected.vertices[0], projected.vertices[1], projected.vertices[2]))
                {
                    unsigned idx = (unsigned)task->triangles.size();
                    int minY = Min(Min((int)projected.vertices[0].y, (int)projected.vertices[1].y), (int)projected.vertices[2].y);
                    int maxY = Max(Max((int)projected.vertices[0].y, (int)projected.vertices[1].y), (int)projected.vertices[2].y);

                    projected.gradients.Calculate(projected.vertices);
                    task->triangles.push_back(projected);

                    // Add to needed slices
                    for (int j = 0; j < activeSlices; ++j)
                    {
                        int startY = j * sliceHeight;
                        int endY = startY + sliceHeight;
                        if (minY < endY && maxY >= startY)
                            task->triangleIndices[j].push_back(idx);
                    }
                }
            }
        }
    }
}

void OcclusionBuffer::ClipVertices(const Vector4& plane, Vector4* vertices, bool* clipTriangles, size_t& numClipTriangles)
{
    size_t trianglesNow = numClipTriangles;
    
    for (size_t i = 0; i < trianglesNow; ++i)
    {
        if (clipTriangles[i])
        {
            size_t index = i * 3;
            float d0 = plane.DotProduct(vertices[index]);
            float d1 = plane.DotProduct(vertices[index + 1]);
            float d2 = plane.DotProduct(vertices[index + 2]);
            
            // If all vertices behind the plane, reject triangle
            if (d0 < 0.0f && d1 < 0.0f && d2 < 0.0f)
            {
                clipTriangles[i] = false;
                continue;
            }
            // If 2 vertices behind the plane, create a new triangle in-place
            else if (d0 < 0.0f && d1 < 0.0f)
            {
                vertices[index] = ClipEdge(vertices[index], vertices[index + 2], d0, d2);
                vertices[index + 1] = ClipEdge(vertices[index + 1], vertices[index + 2], d1, d2);
            }
            else if (d0 < 0.0f && d2 < 0.0f)
            {
                vertices[index] = ClipEdge(vertices[index], vertices[index + 1], d0, d1);
                vertices[index + 2] = ClipEdge(vertices[index + 2], vertices[index + 1], d2, d1);
            }
            else if (d1 < 0.0f && d2 < 0.0f)
            {
                vertices[index + 1] = ClipEdge(vertices[index + 1], vertices[index], d1, d0);
                vertices[index + 2] = ClipEdge(vertices[index + 2], vertices[index], d2, d0);
            }
            // 1 vertex behind the plane: create one new triangle, and modify one in-place
            else if (d0 < 0.0f)
            {
                size_t newIdx = numClipTriangles * 3;
                clipTriangles[numClipTriangles] = true;
                ++numClipTriangles;
                
                vertices[newIdx] = ClipEdge(vertices[index], vertices[index + 2], d0, d2);
                vertices[newIdx + 1] = vertices[index] = ClipEdge(vertices[index], vertices[index + 1], d0, d1);
                vertices[newIdx + 2] = vertices[index + 2];
            }
            else if (d1 < 0.0f)
            {
                size_t newIdx = numClipTriangles * 3;
                clipTriangles[numClipTriangles] = true;
                ++numClipTriangles;
                
                vertices[newIdx + 1] = ClipEdge(vertices[index + 1], vertices[index], d1, d0);
                vertices[newIdx + 2] = vertices[index + 1] = ClipEdge(vertices[index + 1], vertices[index + 2], d1, d2);
                vertices[newIdx] = vertices[index];
            }
            else if (d2 < 0.0f)
            {
                size_t newIdx = numClipTriangles * 3;
                clipTriangles[numClipTriangles] = true;
                ++numClipTriangles;
                
                vertices[newIdx + 2] = ClipEdge(vertices[index + 2], vertices[index + 1], d2, d1);
                vertices[newIdx] = vertices[index + 2] = ClipEdge(vertices[index + 2], vertices[index], d2, d0);
                vertices[newIdx + 1] = vertices[index + 1];
            }
        }
    }
}

void OcclusionBuffer::GenerateTrianglesWork(Task* task, unsigned)
{
    ZoneScoped;

    GenerateTrianglesTask* trianglesTask = static_cast<GenerateTrianglesTask*>(task);
    const TriangleDrawBatch& batch = trianglesTask->batch;
    Matrix4 modelViewProj = viewProj * batch.worldTransform;

    // Theoretical max. amount of vertices if each of the 6 clipping planes doubles the triangle count
    Vector4 vertices[64 * 3];

    if (!batch.indexData)
    {
        // Non-indexed
        unsigned char* srcData = (unsigned char*)batch.vertexData;
        size_t index = 0;

        while (index + 2 < batch.drawCount)
        {
            const Vector3& v0 = *((const Vector3*)(&srcData[index * batch.vertexSize]));
            const Vector3& v1 = *((const Vector3*)(&srcData[(index + 1) * batch.vertexSize]));
            const Vector3& v2 = *((const Vector3*)(&srcData[(index + 2) * batch.vertexSize]));

            vertices[0] = ModelTransform(modelViewProj, v0);
            vertices[1] = ModelTransform(modelViewProj, v1);
            vertices[2] = ModelTransform(modelViewProj, v2);
            AddTriangle(trianglesTask, vertices);

            index += 3;
        }
    }
    else
    {
        // 16-bit indices
        if (batch.indexSize == sizeof(unsigned short))
        {
            unsigned char* srcData = (unsigned char*)batch.vertexData;
            const unsigned short* indices = (const unsigned short*)batch.indexData;
            const unsigned short* indicesEnd = indices + batch.drawCount;

            while (indices < indicesEnd)
            {
                const Vector3& v0 = *((const Vector3*)(&srcData[indices[0] * batch.vertexSize]));
                const Vector3& v1 = *((const Vector3*)(&srcData[indices[1] * batch.vertexSize]));
                const Vector3& v2 = *((const Vector3*)(&srcData[indices[2] * batch.vertexSize]));

                vertices[0] = ModelTransform(modelViewProj, v0);
                vertices[1] = ModelTransform(modelViewProj, v1);
                vertices[2] = ModelTransform(modelViewProj, v2);
                AddTriangle(trianglesTask, vertices);

                indices += 3;
            }
        }
        else
        {
            unsigned char* srcData = (unsigned char*)batch.vertexData;
            const unsigned* indices = (const unsigned*)batch.indexData;
            const unsigned* indicesEnd = indices + batch.drawCount;

            while (indices < indicesEnd)
            {
                const Vector3& v0 = *((const Vector3*)(&srcData[indices[0] * batch.vertexSize]));
                const Vector3& v1 = *((const Vector3*)(&srcData[indices[1] * batch.vertexSize]));
                const Vector3& v2 = *((const Vector3*)(&srcData[indices[2] * batch.vertexSize]));

                vertices[0] = ModelTransform(modelViewProj, v0);
                vertices[1] = ModelTransform(modelViewProj, v1);
                vertices[2] = ModelTransform(modelViewProj, v2);
                AddTriangle(trianglesTask, vertices);

                indices += 3;
            }
        }
    }

    // Start rasterization once triangles for all batches have been generated
    if (numPendingGenerateTasks.fetch_add(-1) == 1)
    {
        numPendingRasterizeTasks.store(activeSlices);
        workQueue->QueueTasks(activeSlices, reinterpret_cast<Task**>(&rasterizeTrianglesTasks[0]));
    }
}

void OcclusionBuffer::RasterizeTrianglesWork(Task* task, unsigned)
{
    ZoneScoped;

    RasterizeTrianglesTask* rasterizeTask = static_cast<RasterizeTrianglesTask*>(task);
    int sliceStartY = rasterizeTask->startY;
    int sliceEndY = rasterizeTask->endY;

    for (int y = sliceStartY; y < sliceEndY; ++y)
    {
        int* start = buffer + width * y;
        int* end = buffer + width * y + width;

        while (start < end)
            *start++ = (int)OCCLUSION_Z_SCALE;
    }

    for (size_t i = 0; i < numTriangleBatches; ++i)
    {
        GenerateTrianglesTask* trianglesTask = generateTrianglesTasks[i];
        const std::vector<GradientTriangle>& triangles = trianglesTask->triangles;
        const std::vector<unsigned>& indices = trianglesTask->triangleIndices[rasterizeTask->sliceIdx];

        for (auto it = indices.begin(); it != indices.end(); ++it)
        {
            unsigned idx = *it;

            const Vector3* vertices = triangles[idx].vertices;
            const Gradients& gradients = triangles[idx].gradients;

            int top, middle, bottom;
            bool middleIsRight;
    
            // Sort vertices in Y-direction
            if (vertices[0].y < vertices[1].y)
            {
                if (vertices[2].y < vertices[0].y)
                {
                    top = 2; middle = 0; bottom = 1; middleIsRight = true;
                }
                else
                {
                    top = 0;
                    if (vertices[1].y < vertices[2].y)
                    {
                        middle = 1; bottom = 2; middleIsRight = true;
                    }
                    else
                    {
                        middle = 2; bottom = 1; middleIsRight = false;
                    }
                }
            }
            else
            {
                if (vertices[2].y < vertices[1].y)
                {
                    top = 2; middle = 1; bottom = 0; middleIsRight = false;
                }
                else
                {
                    top = 1;
                    if (vertices[0].y < vertices[2].y)
                    {
                        middle = 0; bottom = 2; middleIsRight = false;
                    }
                    else
                    {
                        middle = 2; bottom = 0; middleIsRight = true;
                    }
                }
            }
    
            int topY = (int)vertices[top].y;
            int middleY = (int)vertices[middle].y;
            int bottomY = (int)vertices[bottom].y;
    
            // Check for degenerate triangle
            if (topY == bottomY)
                continue;

            Edge topToMiddle(gradients, vertices[top], vertices[middle], topY);
            Edge topToBottom(gradients, vertices[top], vertices[bottom], topY);
            Edge middleToBottom(gradients, vertices[middle], vertices[bottom], middleY);
    
            if (middleIsRight)
            {
                RasterizeSpans(topToBottom, topToMiddle, topY, middleY, gradients.dInvZdXInt, sliceStartY, sliceEndY);
                RasterizeSpans(topToBottom, middleToBottom, middleY, bottomY, gradients.dInvZdXInt, sliceStartY, sliceEndY);
            }
            else
            {
                RasterizeSpans(topToMiddle, topToBottom, topY, middleY, gradients.dInvZdXInt, sliceStartY, sliceEndY);
                RasterizeSpans(middleToBottom, topToBottom, middleY, bottomY, gradients.dInvZdXInt, sliceStartY, sliceEndY);
            }
        }
    }

    // If done, build depth hierarchy
    if (numPendingRasterizeTasks.fetch_add(-1) == 1)
        workQueue->QueueTask(depthHierarchyTask);
}

void OcclusionBuffer::BuildDepthHierarchyWork(Task*, unsigned)
{
    ZoneScoped;

    // Build the first mip level from the pixel-level data
    int mipWidth = (width + 1) / 2;
    int mipHeight = (height + 1) / 2;

    for (int y = 0; y < mipHeight; ++y)
    {
        int* src = buffer + (y * 2) * width;
        DepthValue* dest = mipBuffers[0] + y * mipWidth;
        DepthValue* end = dest + mipWidth;

        if (y * 2 + 1 < height)
        {
            int* src2 = src + width;
            while (dest < end)
            {
                int minUpper = Min(src[0], src[1]);
                int minLower = Min(src2[0], src2[1]);
                dest->min = Min(minUpper, minLower);
                int maxUpper = Max(src[0], src[1]);
                int maxLower = Max(src2[0], src2[1]);
                dest->max = Max(maxUpper, maxLower);

                src += 2;
                src2 += 2;
                ++dest;
            }
        }
        else
        {
            while (dest < end)
            {
                dest->min = Min(src[0], src[1]);
                dest->max = Max(src[0], src[1]);

                src += 2;
                ++dest;
            }
        }
    }

    ++numReadyMipBuffers;

    // Build the rest of the mip levels
    for (size_t i = 1; i < mipBuffers.size(); ++i)
    {
        int prevWidth = mipWidth;
        int prevHeight = mipHeight;
        mipWidth = (mipWidth + 1) / 2;
        mipHeight = (mipHeight + 1) / 2;

        for (int y = 0; y < mipHeight; ++y)
        {
            DepthValue* src = mipBuffers[i - 1] + (y * 2) * prevWidth;
            DepthValue* dest = mipBuffers[i] + y * mipWidth;
            DepthValue* end = dest + mipWidth;

            if (y * 2 + 1 < prevHeight)
            {
                DepthValue* src2 = src + prevWidth;
                while (dest < end)
                {
                    int minUpper = Min(src[0].min, src[1].min);
                    int minLower = Min(src2[0].min, src2[1].min);
                    dest->min = Min(minUpper, minLower);
                    int maxUpper = Max(src[0].max, src[1].max);
                    int maxLower = Max(src2[0].max, src2[1].max);
                    dest->max = Max(maxUpper, maxLower);

                    src += 2;
                    src2 += 2;
                    ++dest;
                }
            }
            else
            {
                while (dest < end)
                {
                    dest->min = Min(src[0].min, src[1].min);
                    dest->max = Max(src[0].max, src[1].max);

                    src += 2;
                    ++dest;
                }
            }
        }

        ++numReadyMipBuffers;
    }
}
