/*
* Copyright (c) 2009 Erin Catto http://www.box2d.org
*
* This software is provided 'as-is', without any express or implied
* warranty.  In no event will the authors be held liable for any damages
* arising from the use of this software.
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 3. This notice may not be removed or altered from any source distribution.
*/

#include <Box2D/Collision/b2DynamicTree.h>
#include <cstring>
#include <cfloat>
using namespace std;


#if B2_USE_DYNAMIC_TREE

b2DynamicTree::b2DynamicTree()
{
	m_root = b2_nullNode;

	m_nodeCapacity = 16;
	m_nodeCount = 0;
	m_nodes = (b2TreeNode*)b2Alloc(m_nodeCapacity * sizeof(b2TreeNode));
	memset(m_nodes, 0, m_nodeCapacity * sizeof(b2TreeNode));

	// Build a linked list for the free list.
	for (int32 i = 0; i < m_nodeCapacity - 1; ++i)
	{
		m_nodes[i].next = i + 1;
		m_nodes[i].height = -1;
	}
	m_nodes[m_nodeCapacity-1].next = b2_nullNode;
	m_nodes[m_nodeCapacity-1].height = -1;
	m_freeList = 0;

	m_path = 0;

	m_insertionCount = 0;
}

b2DynamicTree::~b2DynamicTree()
{
	// This frees the entire tree in one shot.
	b2Free(m_nodes);
}

// Allocate a node from the pool. Grow the pool if necessary.
int32 b2DynamicTree::AllocateNode()
{
	// Expand the node pool as needed.
	if (m_freeList == b2_nullNode)
	{
		b2Assert(m_nodeCount == m_nodeCapacity);

		// The free list is empty. Rebuild a bigger pool.
		b2TreeNode* oldNodes = m_nodes;
		m_nodeCapacity *= 2;
		m_nodes = (b2TreeNode*)b2Alloc(m_nodeCapacity * sizeof(b2TreeNode));
		memcpy(m_nodes, oldNodes, m_nodeCount * sizeof(b2TreeNode));
		b2Free(oldNodes);

		// Build a linked list for the free list. The parent
		// pointer becomes the "next" pointer.
		for (int32 i = m_nodeCount; i < m_nodeCapacity - 1; ++i)
		{
			m_nodes[i].next = i + 1;
			m_nodes[i].height = -1;
		}
		m_nodes[m_nodeCapacity-1].next = b2_nullNode;
		m_nodes[m_nodeCapacity-1].height = -1;
		m_freeList = m_nodeCount;
	}

	// Peel a node off the free list.
	int32 nodeId = m_freeList;
	m_freeList = m_nodes[nodeId].next;
	m_nodes[nodeId].parent = b2_nullNode;
	m_nodes[nodeId].child1 = b2_nullNode;
	m_nodes[nodeId].child2 = b2_nullNode;
	m_nodes[nodeId].height = 0;
	++m_nodeCount;
	return nodeId;
}

// Return a node to the pool.
void b2DynamicTree::FreeNode(int32 nodeId)
{
	b2Assert(0 <= nodeId && nodeId < m_nodeCapacity);
	b2Assert(0 < m_nodeCount);
	m_nodes[nodeId].next = m_freeList;
	m_nodes[nodeId].height = -1;
	m_freeList = nodeId;
	--m_nodeCount;
}

// Create a proxy in the tree as a leaf node. We return the index
// of the node instead of a pointer so that we can grow
// the node pool.
int32 b2DynamicTree::CreateProxy(const b2AABB& aabb, void* userData)
{
	int32 proxyId = AllocateNode();

	// Fatten the aabb.
	b2Vec2 r(b2_aabbExtension, b2_aabbExtension);
	m_nodes[proxyId].aabb.lowerBound = aabb.lowerBound - r;
	m_nodes[proxyId].aabb.upperBound = aabb.upperBound + r;
	m_nodes[proxyId].userData = userData;
	m_nodes[proxyId].height = 0;

	InsertLeaf(proxyId);

	return proxyId;
}

void b2DynamicTree::DestroyProxy(int32 proxyId)
{
	b2Assert(0 <= proxyId && proxyId < m_nodeCapacity);
	b2Assert(m_nodes[proxyId].IsLeaf());

	RemoveLeaf(proxyId);
	FreeNode(proxyId);
}

bool b2DynamicTree::MoveProxy(int32 proxyId, const b2AABB& aabb, const b2Vec2& displacement)
{
	b2Assert(0 <= proxyId && proxyId < m_nodeCapacity);

	b2Assert(m_nodes[proxyId].IsLeaf());

	if (m_nodes[proxyId].aabb.Contains(aabb))
	{
		return false;
	}

	RemoveLeaf(proxyId);

	// Extend AABB.
	b2AABB b = aabb;
	b2Vec2 r(b2_aabbExtension, b2_aabbExtension);
	b.lowerBound = b.lowerBound - r;
	b.upperBound = b.upperBound + r;

	// Predict AABB displacement.
	b2Vec2 d = b2_aabbMultiplier * displacement;

	if (d.x < 0.0f)
	{
		b.lowerBound.x += d.x;
	}
	else
	{
		b.upperBound.x += d.x;
	}

	if (d.y < 0.0f)
	{
		b.lowerBound.y += d.y;
	}
	else
	{
		b.upperBound.y += d.y;
	}

	m_nodes[proxyId].aabb = b;

	InsertLeaf(proxyId);
	return true;
}

void b2DynamicTree::InsertLeaf(int32 leaf)
{
	++m_insertionCount;

	if (m_root == b2_nullNode)
	{
		m_root = leaf;
		m_nodes[m_root].parent = b2_nullNode;
		return;
	}

	// Find the best sibling for this node
	b2AABB leafAABB = m_nodes[leaf].aabb;
	int32 index = m_root;
	while (m_nodes[index].IsLeaf() == false)
	{
		int32 child1 = m_nodes[index].child1;
		int32 child2 = m_nodes[index].child2;

		float32 area = m_nodes[index].aabb.GetPerimeter();

		b2AABB combinedAABB;
		combinedAABB.Combine(m_nodes[index].aabb, leafAABB);
		float32 combinedArea = combinedAABB.GetPerimeter();

		// Cost of creating a new parent for this node and the new leaf
		float32 cost = 2.0f * combinedArea;

		// Minimum cost of pushing the leaf further down the tree
		float32 inheritanceCost = 2.0f * (combinedArea - area);

		// Cost of descending into child1
		float32 cost1;
		if (m_nodes[child1].IsLeaf())
		{
			b2AABB aabb;
			aabb.Combine(leafAABB, m_nodes[child1].aabb);
			cost1 = aabb.GetPerimeter() + inheritanceCost;
		}
		else
		{
			b2AABB aabb;
			aabb.Combine(leafAABB, m_nodes[child1].aabb);
			float32 oldArea = m_nodes[child1].aabb.GetPerimeter();
			float32 newArea = aabb.GetPerimeter();
			cost1 = (newArea - oldArea) + inheritanceCost;
		}

		// Cost of descending into child2
		float32 cost2;
		if (m_nodes[child2].IsLeaf())
		{
			b2AABB aabb;
			aabb.Combine(leafAABB, m_nodes[child2].aabb);
			cost2 = aabb.GetPerimeter() + inheritanceCost;
		}
		else
		{
			b2AABB aabb;
			aabb.Combine(leafAABB, m_nodes[child2].aabb);
			float32 oldArea = m_nodes[child2].aabb.GetPerimeter();
			float32 newArea = aabb.GetPerimeter();
			cost2 = newArea - oldArea + inheritanceCost;
		}

		// Descend according to the minimum cost.
		if (cost < cost1 && cost < cost2)
		{
			break;
		}

		// Descend
		if (cost1 < cost2)
		{
			index = child1;
		}
		else
		{
			index = child2;
		}
	}

	int32 sibling = index;

	// Create a new parent.
	int32 oldParent = m_nodes[sibling].parent;
	int32 newParent = AllocateNode();
	m_nodes[newParent].parent = oldParent;
	m_nodes[newParent].userData = NULL;
	m_nodes[newParent].aabb.Combine(leafAABB, m_nodes[sibling].aabb);
	m_nodes[newParent].height = m_nodes[sibling].height + 1;

	if (oldParent != b2_nullNode)
	{
		// The sibling was not the root.
		if (m_nodes[oldParent].child1 == sibling)
		{
			m_nodes[oldParent].child1 = newParent;
		}
		else
		{
			m_nodes[oldParent].child2 = newParent;
		}

		m_nodes[newParent].child1 = sibling;
		m_nodes[newParent].child2 = leaf;
		m_nodes[sibling].parent = newParent;
		m_nodes[leaf].parent = newParent;
	}
	else
	{
		// The sibling was the root.
		m_nodes[newParent].child1 = sibling;
		m_nodes[newParent].child2 = leaf;
		m_nodes[sibling].parent = newParent;
		m_nodes[leaf].parent = newParent;
		m_root = newParent;
	}

	// Walk back up the tree fixing heights and AABBs
	index = m_nodes[leaf].parent;
	while (index != b2_nullNode)
	{
		index = Balance(index);

		int32 child1 = m_nodes[index].child1;
		int32 child2 = m_nodes[index].child2;

		b2Assert(child1 != b2_nullNode);
		b2Assert(child2 != b2_nullNode);

		m_nodes[index].height = 1 + b2Max(m_nodes[child1].height, m_nodes[child2].height);
		m_nodes[index].aabb.Combine(m_nodes[child1].aabb, m_nodes[child2].aabb);

		index = m_nodes[index].parent;
	}

	//Validate();
}

void b2DynamicTree::RemoveLeaf(int32 leaf)
{
	if (leaf == m_root)
	{
		m_root = b2_nullNode;
		return;
	}

	int32 parent = m_nodes[leaf].parent;
	int32 grandParent = m_nodes[parent].parent;
	int32 sibling;
	if (m_nodes[parent].child1 == leaf)
	{
		sibling = m_nodes[parent].child2;
	}
	else
	{
		sibling = m_nodes[parent].child1;
	}

	if (grandParent != b2_nullNode)
	{
		// Destroy parent and connect sibling to grandParent.
		if (m_nodes[grandParent].child1 == parent)
		{
			m_nodes[grandParent].child1 = sibling;
		}
		else
		{
			m_nodes[grandParent].child2 = sibling;
		}
		m_nodes[sibling].parent = grandParent;
		FreeNode(parent);

		// Adjust ancestor bounds.
		int32 index = grandParent;
		while (index != b2_nullNode)
		{
			index = Balance(index);

			int32 child1 = m_nodes[index].child1;
			int32 child2 = m_nodes[index].child2;

			m_nodes[index].aabb.Combine(m_nodes[child1].aabb, m_nodes[child2].aabb);
			m_nodes[index].height = 1 + b2Max(m_nodes[child1].height, m_nodes[child2].height);

			index = m_nodes[index].parent;
		}
	}
	else
	{
		m_root = sibling;
		m_nodes[sibling].parent = b2_nullNode;
		FreeNode(parent);
	}

	//Validate();
}

// Perform a left or right rotation if node A is imbalanced.
// Returns the new root index.
int32 b2DynamicTree::Balance(int32 iA)
{
	b2Assert(iA != b2_nullNode);

	b2TreeNode* A = m_nodes + iA;
	if (A->IsLeaf() || A->height < 2)
	{
		return iA;
	}

	int32 iB = A->child1;
	int32 iC = A->child2;
	b2Assert(0 <= iB && iB < m_nodeCapacity);
	b2Assert(0 <= iC && iC < m_nodeCapacity);

	b2TreeNode* B = m_nodes + iB;
	b2TreeNode* C = m_nodes + iC;

	int32 balance = C->height - B->height;

	// Rotate C up
	if (balance > 1)
	{
		int32 iF = C->child1;
		int32 iG = C->child2;
		b2TreeNode* F = m_nodes + iF;
		b2TreeNode* G = m_nodes + iG;
		b2Assert(0 <= iF && iF < m_nodeCapacity);
		b2Assert(0 <= iG && iG < m_nodeCapacity);

		// Swap A and C
		C->child1 = iA;
		C->parent = A->parent;
		A->parent = iC;

		// A's old parent should point to C
		if (C->parent != b2_nullNode)
		{
			if (m_nodes[C->parent].child1 == iA)
			{
				m_nodes[C->parent].child1 = iC;
			}
			else
			{
				b2Assert(m_nodes[C->parent].child2 == iA);
				m_nodes[C->parent].child2 = iC;
			}
		}
		else
		{
			m_root = iC;
		}

		// Rotate
		if (F->height > G->height)
		{
			C->child2 = iF;
			A->child2 = iG;
			G->parent = iA;
			A->aabb.Combine(B->aabb, G->aabb);
			C->aabb.Combine(A->aabb, F->aabb);

			A->height = 1 + b2Max(B->height, G->height);
			C->height = 1 + b2Max(A->height, F->height);
		}
		else
		{
			C->child2 = iG;
			A->child2 = iF;
			F->parent = iA;
			A->aabb.Combine(B->aabb, F->aabb);
			C->aabb.Combine(A->aabb, G->aabb);

			A->height = 1 + b2Max(B->height, F->height);
			C->height = 1 + b2Max(A->height, G->height);
		}

		return iC;
	}
	
	// Rotate B up
	if (balance < -1)
	{
		int32 iD = B->child1;
		int32 iE = B->child2;
		b2TreeNode* D = m_nodes + iD;
		b2TreeNode* E = m_nodes + iE;
		b2Assert(0 <= iD && iD < m_nodeCapacity);
		b2Assert(0 <= iE && iE < m_nodeCapacity);

		// Swap A and B
		B->child1 = iA;
		B->parent = A->parent;
		A->parent = iB;

		// A's old parent should point to B
		if (B->parent != b2_nullNode)
		{
			if (m_nodes[B->parent].child1 == iA)
			{
				m_nodes[B->parent].child1 = iB;
			}
			else
			{
				b2Assert(m_nodes[B->parent].child2 == iA);
				m_nodes[B->parent].child2 = iB;
			}
		}
		else
		{
			m_root = iB;
		}

		// Rotate
		if (D->height > E->height)
		{
			B->child2 = iD;
			A->child1 = iE;
			E->parent = iA;
			A->aabb.Combine(C->aabb, E->aabb);
			B->aabb.Combine(A->aabb, D->aabb);

			A->height = 1 + b2Max(C->height, E->height);
			B->height = 1 + b2Max(A->height, D->height);
		}
		else
		{
			B->child2 = iE;
			A->child1 = iD;
			D->parent = iA;
			A->aabb.Combine(C->aabb, D->aabb);
			B->aabb.Combine(A->aabb, E->aabb);

			A->height = 1 + b2Max(C->height, D->height);
			B->height = 1 + b2Max(A->height, E->height);
		}

		return iB;
	}

	return iA;
}

void b2DynamicTree::Rebalance(int32 iterations)
{
	if (m_root == b2_nullNode)
	{
		return;
	}

	// Rebalance the tree by shuffling.
	for (int32 i = 0; i < iterations; ++i)
	{
		while (m_nodes[m_path].height == -1)
		{
			++m_path;
			if (m_path == m_nodeCapacity)
			{
				m_path = 0;
			}
		}

		Shuffle(m_path);

		++m_path;
		if (m_path == m_nodeCapacity)
		{
			m_path = 0;
		}
	}
}

// Shuffle grandchildren to improve quality. This cannot increase the tree height,
// but it can cause slight imbalance.
// Balanced Hierarchies for Collision Detection between Fracturing Objects
// http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.125.7818&rep=rep1&type=pdf
void b2DynamicTree::Shuffle(int32 index)
{
	if (index == b2_nullNode)
	{
		return;
	}

	b2TreeNode* node = m_nodes + index;
	if (node->height < 2)
	{
		return;
	}

	int32 i1 = node->child1;
	int32 i2 = node->child2;

	b2Assert(0 <= i1 && i1 < m_nodeCapacity);
	b2Assert(0 <= i2 && i2 < m_nodeCapacity);

	b2TreeNode* node1 = m_nodes + i1;
	b2TreeNode* node2 = m_nodes + i2;

	if (node1->height < 1 || node2->height < 1)
	{
		return;
	}

	int32 i11 = node1->child1;
	int32 i12 = node1->child2;
	int32 i21 = node2->child1;
	int32 i22 = node2->child2;

	b2TreeNode* node11 = m_nodes + i11;
	b2TreeNode* node12 = m_nodes + i12;
	b2TreeNode* node21 = m_nodes + i21;
	b2TreeNode* node22 = m_nodes + i22;

	b2AABB b11 = node11->aabb;
	b2AABB b12 = node12->aabb;
	b2AABB b21 = node21->aabb;
	b2AABB b22 = node22->aabb;

	// Metrics
	float32 m1, m2, m3;

	{
		b2AABB b1, b2;
		b1.Combine(b11, b12);
		b2.Combine(b21, b22);
		m1 = b1.GetPerimeter() + b2.GetPerimeter();
	}

	{
		b2AABB b1, b2;
		b1.Combine(b11, b22);
		b2.Combine(b12, b21);
		m2 = b1.GetPerimeter() + b2.GetPerimeter();
	}

	{
		b2AABB b1, b2;
		b1.Combine(b11, b21);
		b2.Combine(b12, b22);
		m3 = b1.GetPerimeter() + b2.GetPerimeter();
	}

	if (m1 <= m2 && m1 <= m3)
	{
		return;
	}

	if (m2 <= m3)
	{
		// (node11, node22), (node21, node12)
		node1->child2 = i22;
		node22->parent = i1;
		node1->aabb.Combine(node11->aabb, node22->aabb);
		node1->height = 1 + b2Max(node11->height, node22->height);

		node2->child2 = i12;
		node12->parent = i2;
		node2->aabb.Combine(node21->aabb, node12->aabb);
		node2->height = 1 + b2Max(node21->height, node12->height);
	}
	else
	{
		// (node11, node21), (node12, node22)
		node1->child2 = i21;
		node21->parent = i1;
		node1->aabb.Combine(node11->aabb, node21->aabb);
		node1->height = 1 + b2Max(node11->height, node21->height);

		node2->child1 = i12;
		node12->parent = i2;
		node2->aabb.Combine(node12->aabb, node22->aabb);
		node2->height = 1 + b2Max(node12->height, node22->height);
	}

	node->aabb.Combine(node1->aabb, node2->aabb);
	node->height = 1 + b2Max(node1->height, node2->height);

	int32 i = node->parent;
	while (i != b2_nullNode)
	{
		b2TreeNode* n = m_nodes + i;
		
		int32 i1 = n->child1;
		int32 i2 = n->child2;

		b2TreeNode* n1 = m_nodes + i1;
		b2TreeNode* n2 = m_nodes + i2;

		n->aabb.Combine(n1->aabb, n2->aabb);
		n->height = 1 + b2Max(n1->height, n2->height);

		i = n->parent;
	}

	//Validate();
}

int32 b2DynamicTree::GetHeight() const
{
	if (m_root == b2_nullNode)
	{
		return 0;
	}

	return m_nodes[m_root].height;
}

// Compute the total surface area of a sub-tree (perimeter)
float32 b2DynamicTree::GetTotalArea(int32 index) const
{
	if (index == b2_nullNode)
	{
		return 0.0f;
	}

	b2Assert(0 <= index && index < m_nodeCapacity);
	const b2TreeNode* node = m_nodes + index;
	float32 area = node->aabb.GetPerimeter();
	float32 area1 = GetTotalArea(node->child1);
	float32 area2 = GetTotalArea(node->child2);
	return area + area1 + area2;
}

//
float32 b2DynamicTree::GetAreaRatio() const
{
	if (m_root == b2_nullNode)
	{
		return 0.0f;
	}

	float32 totalArea = GetTotalArea(m_root);
	const b2TreeNode* root = m_nodes + m_root;
	float32 rootArea = root->aabb.GetPerimeter();

	return totalArea / rootArea;
}

// Compute the height of a sub-tree.
int32 b2DynamicTree::ComputeHeight(int32 nodeId) const
{
	b2Assert(0 <= nodeId && nodeId < m_nodeCapacity);
	b2TreeNode* node = m_nodes + nodeId;

	if (node->IsLeaf())
	{
		return 0;
	}

	int32 height1 = ComputeHeight(node->child1);
	int32 height2 = ComputeHeight(node->child2);
	return 1 + b2Max(height1, height2);
}

int32 b2DynamicTree::ComputeHeight() const
{
	int32 height = ComputeHeight(m_root);
	return height;
}

void b2DynamicTree::ValidateStructure(int32 index) const
{
	if (index == b2_nullNode)
	{
		return;
	}

	if (index == m_root)
	{
		b2Assert(m_nodes[index].parent == b2_nullNode);
	}

	const b2TreeNode* node = m_nodes + index;

	int32 child1 = node->child1;
	int32 child2 = node->child2;

	if (node->IsLeaf())
	{
		b2Assert(child1 == b2_nullNode);
		b2Assert(child2 == b2_nullNode);
		b2Assert(node->height == 0);
		return;
	}

	b2Assert(0 <= child1 && child1 < m_nodeCapacity);
	b2Assert(0 <= child2 && child2 < m_nodeCapacity);

	b2Assert(m_nodes[child1].parent == index);
	b2Assert(m_nodes[child2].parent == index);

	ValidateStructure(child1);
	ValidateStructure(child2);
}

void b2DynamicTree::ValidateMetrics(int32 index) const
{
	if (index == b2_nullNode)
	{
		return;
	}

	const b2TreeNode* node = m_nodes + index;

	int32 child1 = node->child1;
	int32 child2 = node->child2;

	if (node->IsLeaf())
	{
		b2Assert(child1 == b2_nullNode);
		b2Assert(child2 == b2_nullNode);
		b2Assert(node->height == 0);
		return;
	}

	b2Assert(0 <= child1 && child1 < m_nodeCapacity);
	b2Assert(0 <= child2 && child2 < m_nodeCapacity);

	int32 height1 = m_nodes[child1].height;
	int32 height2 = m_nodes[child2].height;
	int32 height = 1 + b2Max(height1, height2);
	b2Assert(node->height == height);

	b2AABB aabb;
	aabb.Combine(m_nodes[child1].aabb, m_nodes[child2].aabb);

	b2Assert(aabb.lowerBound == node->aabb.lowerBound);
	b2Assert(aabb.upperBound == node->aabb.upperBound);

	ValidateMetrics(child1);
	ValidateMetrics(child2);
}

void b2DynamicTree::Validate() const
{
	ValidateStructure(m_root);
	ValidateMetrics(m_root);

	int32 freeCount = 0;
	int32 freeIndex = m_freeList;
	while (freeIndex != b2_nullNode)
	{
		b2Assert(0 <= freeIndex && freeIndex < m_nodeCapacity);
		freeIndex = m_nodes[freeIndex].next;
		++freeCount;
	}

	b2Assert(GetHeight() == ComputeHeight());

	b2Assert(m_nodeCount + freeCount == m_nodeCapacity);
}

int32 b2DynamicTree::GetMaxBalance() const
{
	return GetMaxBalance(m_root);
}

int32 b2DynamicTree::GetMaxBalance(int32 index) const
{
	if (index == b2_nullNode)
	{
		return 0;
	}

	if (m_nodes[index].IsLeaf())
	{
		return 0;
	}

	int32 child1 = m_nodes[index].child1;
	int32 child2 = m_nodes[index].child2;

	int32 balance = b2Abs(m_nodes[child2].height - m_nodes[child1].height);
	int32 balance1 = GetMaxBalance(child1);
	int32 balance2 = GetMaxBalance(child2);

	return b2Max(balance, b2Max(balance1, balance2));
}

#elif B2_USE_BRUTE_FORCE

b2DynamicTree::b2DynamicTree()
{
	m_proxyCapacity = 128;
	m_proxyCount = 0;

	m_proxyMap = (int32*)b2Alloc(m_proxyCapacity * sizeof(int32));
	m_proxies = (b2Proxy*)b2Alloc(m_proxyCapacity * sizeof(b2Proxy));

	// Build the free list
	m_freeId = 0;
	int32 last = m_proxyCapacity - 1;
	for (int32 i = m_freeId; i < last; ++i)
	{
		m_proxyMap[i] = i + 1;
	}

	m_proxyMap[last] = b2_nullNode;
}

b2DynamicTree::~b2DynamicTree()
{
	b2Free(m_proxyMap);
	b2Free(m_proxies);
}

int32 b2DynamicTree::CreateProxy(const b2AABB& aabb, void* userData)
{
	if (m_proxyCount == m_proxyCapacity)
	{
		m_proxyCapacity *= 2;
		int32* proxyMap = (int32*)b2Alloc(m_proxyCapacity * sizeof(int32));
		b2Proxy* proxies = (b2Proxy*)b2Alloc(m_proxyCapacity * sizeof(b2Proxy));

		memcpy(proxyMap, m_proxyMap, m_proxyCount * sizeof(int32));
		memcpy(proxies, m_proxies, m_proxyCount * sizeof(b2Proxy));

		b2Free(m_proxyMap);
		b2Free(m_proxies);
		m_proxyMap = proxyMap;
		m_proxies = proxies;
		proxyMap = NULL;
		proxies = NULL;

		m_freeId = m_proxyCount;
		int32 last = m_proxyCapacity - 1;
		for (int32 i = m_freeId; i < last; ++i)
		{
			m_proxyMap[i] = i + 1;
		}

		m_proxyMap[last] = b2_nullNode;
	}

	b2Assert(0 <= m_freeId && m_freeId < m_proxyCapacity);
	int32 id = m_freeId;
	m_freeId = m_proxyMap[id];
	int32 index = m_proxyCount;

	m_proxies[index].aabb = aabb;
	m_proxies[index].userData = userData;
	m_proxies[index].id = id;
	m_proxyMap[id] = index;
	++m_proxyCount;

	return id;
}

void b2DynamicTree::DestroyProxy(int32 proxyId)
{
	b2Assert(0 < m_proxyCount && 0 <= proxyId && proxyId < m_proxyCapacity);
	int32 index = m_proxyMap[proxyId];

	// Add to free list
	m_proxyMap[proxyId] = m_freeId;
	m_freeId = proxyId;

	// Keep proxy array contiguous
	if (index < m_proxyCount - 1)
	{
		m_proxies[index] = m_proxies[m_proxyCount-1];
		int32 id = m_proxies[index].id;
		m_proxyMap[id] = index;
	}

	--m_proxyCount;

	Validate();
}

bool b2DynamicTree::MoveProxy(int32 proxyId, const b2AABB& aabb, const b2Vec2& displacement)
{
	b2Assert(0 < m_proxyCount && 0 <= proxyId && proxyId < m_proxyCapacity);
	B2_NOT_USED(displacement);

	int32 index = m_proxyMap[proxyId];

	if (m_proxies[index].aabb.Contains(aabb))
	{
		return false;
	}

	// Extend AABB.
	b2AABB b = aabb;
	b2Vec2 r(b2_aabbExtension, b2_aabbExtension);
	b.lowerBound = b.lowerBound - r;
	b.upperBound = b.upperBound + r;

	// Predict AABB displacement.
	b2Vec2 d = b2_aabbMultiplier * displacement;

	if (d.x < 0.0f)
	{
		b.lowerBound.x += d.x;
	}
	else
	{
		b.upperBound.x += d.x;
	}

	if (d.y < 0.0f)
	{
		b.lowerBound.y += d.y;
	}
	else
	{
		b.upperBound.y += d.y;
	}

	m_proxies[index].aabb = b;

	return true;
}

void b2DynamicTree::Validate() const
{
	b2Assert(m_proxyCount > 0 || m_freeId == b2_nullNode);
	b2Assert(m_freeId == b2_nullNode || m_freeId < m_proxyCapacity);

	int32 id = m_freeId;
	int32 freeCount = 0;
	while (id != b2_nullNode)
	{
		++freeCount;
		b2Assert(freeCount <= m_proxyCapacity);
		id = m_proxyMap[id];
	}

	b2Assert(freeCount + m_proxyCount == m_proxyCapacity);

	b2Assert(m_proxyCount <= m_proxyCapacity);

	for (int32 i = 0; i < m_proxyCount; ++i)
	{
		int32 id = m_proxies[i].id;

		b2Assert(m_proxyMap[id] == i);
	}
}

#endif