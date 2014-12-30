// For conditions of distribution and use, see copyright notice in License.txt

#pragma once

#include "../Base/Allocator.h"
#include "../Debug/Profiler.h"
#include "../Math/BoundingBox.h"
#include "OctreeNode.h"

namespace Turso3D
{

static const size_t NUM_OCTANTS = 8;

class Octree;
class OctreeNode;
class Ray;

/// Structure for raycast query results.
struct TURSO3D_API RaycastResult
{
    /// Hit world position.
    Vector3 position;
    /// Hit world normal.
    Vector3 normal;
    /// Hit distance along the ray.
    float distance;
    /// Hit node.
    OctreeNode* node;
    /// Subclass specific hit details.
    void* extraData;
};

/// %Octree cell, contains up to 8 child octants.
struct TURSO3D_API Octant
{
    /// Construct.
    Octant();
   
    /// Initialize parent and bounds.
    void Initialize(Octant* parent, const BoundingBox& boundingBox, int level);
    /// Test if a node should be inserted in this octant or if a smaller child octant should be created.
    bool FitBoundingBox(const BoundingBox& box, const Vector3& boxSize) const;
    /// Return child octant index based on position.
    size_t ChildIndex(const Vector3& position) const { size_t ret = position.x < center.x ? 0 : 1; ret += position.y < center.y ? 0 : 2; ret += position.z < center.z ? 0 : 4; return ret; }
    
    /// Expanded (loose) bounding box used for culling the octant and the nodes within it.
    BoundingBox cullingBox;
    /// Actual bounding box of the octant.
    BoundingBox worldBoundingBox;
    /// Bounding box center.
    Vector3 center;
    /// Bounding box half size.
    Vector3 halfSize;
    /// Subdivision level.
    int level;
    /// Nodes contained in the octant.
    Vector<OctreeNode*> nodes;
    /// Child octants.
    Octant* children[NUM_OCTANTS];
    /// Parent octant.
    Octant* parent;
    /// Number of nodes in this octant and the child octants combined.
    size_t numNodes;
};

/// Acceleration structure for rendering. Should be created as a child of the scene root.
class TURSO3D_API Octree : public Node
{
    OBJECT(Octree);

public:
    /// Construct.
    Octree();
    /// Destruct. Delete all child octants and detach the nodes.
    ~Octree();

    /// Register factory and attributes.
    static void RegisterObject();
    
    /// Process the queue of nodes to be reinserted.
    void Update();
    /// Resize octree.
    void Resize(const BoundingBox& boundingBox, int numLevels);
    /// Remove a node from the octree.
    void RemoveNode(OctreeNode* node);
    /// Queue a reinsertion for a node.
    void QueueUpdate(OctreeNode* node);
    /// Cancel a pending reinsertion.
    void CancelUpdate(OctreeNode* node);
    /// Query for nodes with a raycast and return all results.
    void Raycast(Vector<RaycastResult>& result, const Ray& ray, unsigned short nodeFlags, float maxDistance = M_INFINITY, unsigned layerMask = LAYERMASK_ALL);
    /// Query for nodes with a raycast and return the closest result.
    RaycastResult RaycastSingle(const Ray& ray, unsigned short nodeFlags, float maxDistance = M_INFINITY, unsigned layerMask = LAYERMASK_ALL);

    /// Query for nodes using a volume such as frustum or sphere.
    template <class T> void FindNodes(Vector<OctreeNode*>& result, const T& volume, unsigned short nodeFlags, unsigned layerMask = LAYERMASK_ALL) const
    {
        PROFILE(QueryOctree);
        CollectNodes(result, &root, volume, nodeFlags, layerMask);
    }

    /// Query for nodes of two kinds (geometries and lights for example) using a volume such as frustum or sphere.
    template <class T> void FindNodes(Vector<OctreeNode*>& result1, unsigned short nodeFlags1, Vector<OctreeNode*>& result2, unsigned short nodeFlags2, const T& volume, unsigned layerMask = LAYERMASK_ALL) const
    {
        PROFILE(QueryOctree);
        CollectNodes(result1, nodeFlags1, result2, nodeFlags2, &root, volume, layerMask);
    }

private:
    /// Set bounding box. Used in serialization.
    void SetBoundingBoxAttr(const BoundingBox& boundingBox);
    /// Return bounding box. Used in serialization.
    const BoundingBox& BoundingBoxAttr() const;
    /// Set number of levels. Used in serialization.
    void SetNumLevelsAttr(int numLevels);
    /// Return number of levels. Used in serialization.
    int NumLevelsAttr() const;
    /// Add node to a specific octant.
    void AddNode(OctreeNode* node, Octant* octant);
    /// Remove node from an octant.
    void RemoveNode(OctreeNode* node, Octant* octant);
    /// Create a new child octant.
    Octant* CreateChildOctant(Octant* octant, size_t index);
    /// Delete one child octant.
    void DeleteChildOctant(Octant* octant, size_t index);
    /// Delete a child octant hierarchy. If not deleting the octree for good, moves any nodes back to the root octant.
    void DeleteChildOctants(Octant* octant, bool deletingOctree);
    /// Get all nodes from an octant recursively.
    void CollectNodes(Vector<OctreeNode*>& result, const Octant* octant) const;
    /// Get all visible nodes matching flags from an octant recursively.
    void CollectNodes(Vector<OctreeNode*>& result, const Octant* octant, unsigned short nodeFlags, unsigned layerMask) const;
    /// Get visible nodes of two kinds from an octant recursively.
    void CollectNodes(Vector<OctreeNode*>& result1, unsigned short nodeFlags1, Vector<OctreeNode*>& result2, unsigned short nodeFlags2, const Octant* octant, unsigned layerMask) const;
    /// Get all visible nodes matching flags along a ray.
    void CollectNodes(Vector<RaycastResult>& result, const Octant* octant, const Ray& ray, unsigned short nodeFlags, float maxDistance, unsigned layerMask) const;
    /// Get all visible nodes matching flags that could be potential raycast hits.
    void CollectNodes(Vector<Pair<OctreeNode*, float> >& result, const Octant* octant, const Ray& ray, unsigned short nodeFlags, float maxDistance, unsigned layerMask) const;

    /// Collect nodes matching flags using a volume such as frustum or sphere.
    template <class T> void CollectNodes(Vector<OctreeNode*>& result, const Octant* octant, const T& volume, unsigned short nodeFlags, unsigned layerMask) const
    {
        Intersection res = volume.IsInside(octant->cullingBox);
        if (res == OUTSIDE)
            return;
        
        // If this octant is completely inside the volume, can include all contained octants and their nodes without further tests
        if (res == INSIDE)
            CollectNodes(result, octant, nodeFlags, layerMask);
        else
        {
            const Vector<OctreeNode*>& octantNodes = octant->nodes;
            for (auto it = octantNodes.Begin(); it != octantNodes.End(); ++it)
            {
                OctreeNode* node = *it;
                if ((node->Flags() & nodeFlags) == nodeFlags && (node->LayerMask() & layerMask) &&
                    volume.IsInsideFast(node->WorldBoundingBox()) != OUTSIDE)
                    result.Push(node);
            }
            
            for (size_t i = 0; i < NUM_OCTANTS; ++i)
            {
                if (octant->children[i])
                    CollectNodes(result, octant->children[i], volume, nodeFlags, layerMask);
            }
        }
    }

    /// Collect nodes of two kinds using a volume such as frustum or sphere.
    template <class T> void CollectNodes(Vector<OctreeNode*>& result1, unsigned short nodeFlags1, Vector<OctreeNode*>& result2, unsigned short nodeFlags2, const Octant* octant, const T& volume, unsigned layerMask) const
    {
        Intersection res = volume.IsInside(octant->cullingBox);
        if (res == OUTSIDE)
            return;

        // If this octant is completely inside the volume, can include all contained octants and their nodes without further tests
        if (res == INSIDE)
            CollectNodes(result1, nodeFlags1, result2, nodeFlags2, octant, layerMask);
        else
        {
            const Vector<OctreeNode*>& octantNodes = octant->nodes;

            for (auto it = octantNodes.Begin(); it != octantNodes.End(); ++it)
            {
                OctreeNode* node = *it;
                unsigned short flags = node->Flags();
                if (((flags & nodeFlags1) == nodeFlags1 || (flags & nodeFlags2) == nodeFlags2) && (node->LayerMask() & layerMask) &&
                    volume.IsInsideFast(node->WorldBoundingBox()) != OUTSIDE)
                {
                    if ((flags & nodeFlags1) == nodeFlags1)
                        result1.Push(node);
                    else
                        result2.Push(node);
                }
            }

            for (size_t i = 0; i < NUM_OCTANTS; ++i)
            {
                if (octant->children[i])
                    CollectNodes(result1, nodeFlags1, result2, nodeFlags2, octant->children[i], volume, layerMask);
            }
        }
    }
    
    /// Queue of nodes to be reinserted.
    Vector<OctreeNode*> updateQueue;
    /// RaycastSingle initial coarse result.
    Vector<Pair<OctreeNode*, float> > initialRes;
    /// RaycastSingle final result.
    Vector<RaycastResult> finalRes;
    /// Allocator for child octants.
    Allocator<Octant> allocator;
    /// Root octant.
    Octant root;
};

}