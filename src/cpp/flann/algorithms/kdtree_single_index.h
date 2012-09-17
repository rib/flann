/***********************************************************************
 * Software License Agreement (BSD License)
 *
 * Copyright 2008-2009  Marius Muja (mariusm@cs.ubc.ca). All rights reserved.
 * Copyright 2008-2009  David G. Lowe (lowe@cs.ubc.ca). All rights reserved.
 *
 * THE BSD LICENSE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *************************************************************************/

#ifndef FLANN_KDTREE_SINGLE_INDEX_H_
#define FLANN_KDTREE_SINGLE_INDEX_H_

#include <algorithm>
#include <map>
#include <cassert>
#include <cstring>

#include "flann/general.h"
#include "flann/algorithms/nn_index.h"
#include "flann/util/matrix.h"
#include "flann/util/result_set.h"
#include "flann/util/heap.h"
#include "flann/util/allocator.h"
#include "flann/util/random.h"
#include "flann/util/saving.h"

namespace flann
{

struct KDTreeSingleIndexParams : public IndexParams
{
    KDTreeSingleIndexParams(int leaf_max_size = 10, bool reorder = true)
    {
        (*this)["algorithm"] = FLANN_INDEX_KDTREE_SINGLE;
        (*this)["leaf_max_size"] = leaf_max_size;
        (*this)["reorder"] = reorder;
    }
};


/**
 * Single kd-tree index
 *
 * Contains the k-d trees and other information for indexing a set of points
 * for nearest-neighbor matching.
 */
template <typename Distance>
class KDTreeSingleIndex : public NNIndex<KDTreeSingleIndex<Distance>, typename Distance::ElementType, typename Distance::ResultType>
{
public:
    typedef typename Distance::ElementType ElementType;
    typedef typename Distance::ResultType DistanceType;
    typedef NNIndex<KDTreeSingleIndex<Distance>, ElementType, DistanceType> BaseClass;

    typedef bool needs_kdtree_distance;

    /**
     * KDTree constructor
     *
     * Params:
     *          params = parameters passed to the kdtree algorithm
     */
    KDTreeSingleIndex(const IndexParams& params = KDTreeSingleIndexParams(), Distance d = Distance() ) :
        BaseClass(params), distance_(d)
    {
        leaf_max_size_ = get_param(params,"leaf_max_size",10);
        reorder_ = get_param(params,"reorder",true);
    }

    /**
     * KDTree constructor
     *
     * Params:
     *          inputData = dataset with the input features
     *          params = parameters passed to the kdtree algorithm
     */
    KDTreeSingleIndex(const Matrix<ElementType>& inputData, const IndexParams& params = KDTreeSingleIndexParams(),
                      Distance d = Distance() ) :
        BaseClass(params), distance_(d)
    {
        leaf_max_size_ = get_param(params,"leaf_max_size",10);
        reorder_ = get_param(params,"reorder",true);

        bool copy_dataset = get_param(index_params_, "copy_dataset", false);
        setDataset(inputData, copy_dataset);
    }

    KDTreeSingleIndex(const KDTreeSingleIndex&);
    KDTreeSingleIndex& operator=(const KDTreeSingleIndex&);

    /**
     * Standard destructor
     */
    ~KDTreeSingleIndex()
    {
        if (reorder_) {
            delete[] data_.ptr();
        }
        if (ownDataset_) {
        	delete[] dataset_.ptr();
        }
    }

    /**
     * Builds the index
     */
    void buildIndex()
    {
        // Create a permutable array of indices to the input vectors.
        vind_.resize(size_);
        for (size_t i = 0; i < size_; i++) {
            vind_[i] = i;
        }

        computeBoundingBox(root_bbox_);
        root_node_ = divideTree(0, size_, root_bbox_ );   // construct the tree

        if (reorder_) {
            data_ = flann::Matrix<ElementType>(new ElementType[size_*veclen_], size_, veclen_);
            for (size_t i=0; i<size_; ++i) {
                std::copy(dataset_[vind_[i]], dataset_[vind_[i]]+dataset_.cols, data_[i]);
            }
        }
        else {
            data_ = dataset_;
        }
    }    


    void addPoints(const Matrix<ElementType>& points, float rebuild_threshold = 2)
    {
        assert(points.cols==veclen_);
        extendDataset(points);
        buildIndex();
    }


    flann_algorithm_t getType() const
    {
        return FLANN_INDEX_KDTREE_SINGLE;
    }


    void saveIndex(FILE* stream)
    {
        save_value(stream, size_);
        save_value(stream, veclen_);
        save_value(stream, root_bbox_);
        save_value(stream, reorder_);
        save_value(stream, leaf_max_size_);
        save_value(stream, vind_);
        if (reorder_) {
            save_value(stream, data_);
        }
        save_tree(stream, root_node_);
    }


    void loadIndex(FILE* stream)
    {
        load_value(stream, size_);
        load_value(stream, veclen_);
        load_value(stream, root_bbox_);
        load_value(stream, reorder_);
        load_value(stream, leaf_max_size_);
        load_value(stream, vind_);
        if (reorder_) {
            load_value(stream, data_);
        }
        else {
            data_ = dataset_;
        }
        load_tree(stream, root_node_);


        index_params_["algorithm"] = getType();
        index_params_["leaf_max_size"] = leaf_max_size_;
        index_params_["reorder"] = reorder_;
    }

    /**
     * Computes the inde memory usage
     * Returns: memory used by the index
     */
    int usedMemory() const
    {
        return pool_.usedMemory+pool_.wastedMemory+dataset_.rows*sizeof(int);  // pool memory and vind array memory
    }

    /**
     * Find set of nearest neighbors to vec. Their indices are stored inside
     * the result object.
     *
     * Params:
     *     result = the result object in which the indices of the nearest-neighbors are stored
     *     vec = the vector for which to search the nearest neighbors
     *     maxCheck = the maximum number of restarts (in a best-bin-first manner)
     */
    template <typename ResultSet>
    void findNeighbors(ResultSet& result, const ElementType* vec, const SearchParams& searchParams)
    {
        float epsError = 1+searchParams.eps;

        std::vector<DistanceType> dists(veclen_,0);
        DistanceType distsq = computeInitialDistances(vec, dists);
        searchLevel(result, vec, root_node_, distsq, dists, epsError);
    }

private:


    /*--------------------- Internal Data Structures --------------------------*/
    struct Node
    {
    	/**
    	 * Indices of points in leaf node
    	 */
    	int left, right;
    	/**
    	 * Dimension used for subdivision.
    	 */
    	int divfeat;
    	/**
    	 * The values used for subdivision.
    	 */
    	DistanceType divlow, divhigh;
        /**
         * The child nodes.
         */
        Node* child1, * child2;
    };
    typedef Node* NodePtr;


    struct Interval
    {
        DistanceType low, high;
    };

    typedef std::vector<Interval> BoundingBox;

    typedef BranchStruct<NodePtr, DistanceType> BranchSt;
    typedef BranchSt* Branch;




    void save_tree(FILE* stream, NodePtr tree)
    {
        save_value(stream, *tree);
        if (tree->child1!=NULL) {
            save_tree(stream, tree->child1);
        }
        if (tree->child2!=NULL) {
            save_tree(stream, tree->child2);
        }
    }


    void load_tree(FILE* stream, NodePtr& tree)
    {
        tree = new(pool_) Node();
        load_value(stream, *tree);
        if (tree->child1!=NULL) {
            load_tree(stream, tree->child1);
        }
        if (tree->child2!=NULL) {
            load_tree(stream, tree->child2);
        }
    }


    void computeBoundingBox(BoundingBox& bbox)
    {
        bbox.resize(veclen_);
        for (size_t i=0; i<veclen_; ++i) {
            bbox[i].low = (DistanceType)dataset_[0][i];
            bbox[i].high = (DistanceType)dataset_[0][i];
        }
        for (size_t k=1; k<dataset_.rows; ++k) {
            for (size_t i=0; i<veclen_; ++i) {
                if (dataset_[k][i]<bbox[i].low) bbox[i].low = (DistanceType)dataset_[k][i];
                if (dataset_[k][i]>bbox[i].high) bbox[i].high = (DistanceType)dataset_[k][i];
            }
        }
    }


    /**
     * Create a tree node that subdivides the list of vecs from vind[first]
     * to vind[last].  The routine is called recursively on each sublist.
     * Place a pointer to this new tree node in the location pTree.
     *
     * Params: pTree = the new node to create
     *                  first = index of the first vector
     *                  last = index of the last vector
     */
    NodePtr divideTree(int left, int right, BoundingBox& bbox)
    {
        NodePtr node = new(pool_) Node(); // allocate memory

        /* If too few exemplars remain, then make this a leaf node. */
        if ( (right-left) <= leaf_max_size_) {
            node->child1 = node->child2 = NULL;    /* Mark as leaf node. */
            node->left = left;
            node->right = right;

            // compute bounding-box of leaf points
            for (size_t i=0; i<veclen_; ++i) {
                bbox[i].low = (DistanceType)dataset_[vind_[left]][i];
                bbox[i].high = (DistanceType)dataset_[vind_[left]][i];
            }
            for (int k=left+1; k<right; ++k) {
                for (size_t i=0; i<veclen_; ++i) {
                    if (bbox[i].low>dataset_[vind_[k]][i]) bbox[i].low=(DistanceType)dataset_[vind_[k]][i];
                    if (bbox[i].high<dataset_[vind_[k]][i]) bbox[i].high=(DistanceType)dataset_[vind_[k]][i];
                }
            }
        }
        else {
            int idx;
            int cutfeat;
            DistanceType cutval;
            middleSplit(&vind_[0]+left, right-left, idx, cutfeat, cutval, bbox);

            node->divfeat = cutfeat;

            BoundingBox left_bbox(bbox);
            left_bbox[cutfeat].high = cutval;
            node->child1 = divideTree(left, left+idx, left_bbox);

            BoundingBox right_bbox(bbox);
            right_bbox[cutfeat].low = cutval;
            node->child2 = divideTree(left+idx, right, right_bbox);

            node->divlow = left_bbox[cutfeat].high;
            node->divhigh = right_bbox[cutfeat].low;

            for (size_t i=0; i<veclen_; ++i) {
            	bbox[i].low = std::min(left_bbox[i].low, right_bbox[i].low);
            	bbox[i].high = std::max(left_bbox[i].high, right_bbox[i].high);
            }
        }

        return node;
    }

    void computeMinMax(int* ind, int count, int dim, ElementType& min_elem, ElementType& max_elem)
    {
        min_elem = dataset_[ind[0]][dim];
        max_elem = dataset_[ind[0]][dim];
        for (int i=1; i<count; ++i) {
            ElementType val = dataset_[ind[i]][dim];
            if (val<min_elem) min_elem = val;
            if (val>max_elem) max_elem = val;
        }
    }

    void middleSplit(int* ind, int count, int& index, int& cutfeat, DistanceType& cutval, const BoundingBox& bbox)
    {
        // find the largest span from the approximate bounding box
        ElementType max_span = bbox[0].high-bbox[0].low;
        cutfeat = 0;
        cutval = (bbox[0].high+bbox[0].low)/2;
        for (size_t i=1; i<veclen_; ++i) {
            ElementType span = bbox[i].high-bbox[i].low;
            if (span>max_span) {
                max_span = span;
                cutfeat = i;
                cutval = (bbox[i].high+bbox[i].low)/2;
            }
        }

        // compute exact span on the found dimension
        ElementType min_elem, max_elem;
        computeMinMax(ind, count, cutfeat, min_elem, max_elem);
        cutval = (min_elem+max_elem)/2;
        max_span = max_elem - min_elem;

        // check if a dimension of a largest span exists
        size_t k = cutfeat;
        for (size_t i=0; i<veclen_; ++i) {
            if (i==k) continue;
            ElementType span = bbox[i].high-bbox[i].low;
            if (span>max_span) {
                computeMinMax(ind, count, i, min_elem, max_elem);
                span = max_elem - min_elem;
                if (span>max_span) {
                    max_span = span;
                    cutfeat = i;
                    cutval = (min_elem+max_elem)/2;
                }
            }
        }
        int lim1, lim2;
        planeSplit(ind, count, cutfeat, cutval, lim1, lim2);

        if (lim1>count/2) index = lim1;
        else if (lim2<count/2) index = lim2;
        else index = count/2;

        assert(index > 0 && index < count);
    }


    void middleSplit_(int* ind, int count, int& index, int& cutfeat, DistanceType& cutval, const BoundingBox& bbox)
    {
        const float eps_val=0.00001f;
        DistanceType max_span = bbox[0].high-bbox[0].low;
        for (size_t i=1; i<veclen_; ++i) {
            DistanceType span = bbox[i].high-bbox[i].low;
            if (span>max_span) {
                max_span = span;
            }
        }
        DistanceType max_spread = -1;
        cutfeat = 0;
        for (size_t i=0; i<veclen_; ++i) {
            DistanceType span = bbox[i].high-bbox[i].low;
            if (span>(DistanceType)((1-eps_val)*max_span)) {
                ElementType min_elem, max_elem;
                computeMinMax(ind, count, cutfeat, min_elem, max_elem);
                DistanceType spread = (DistanceType)(max_elem-min_elem);
                if (spread>max_spread) {
                    cutfeat = i;
                    max_spread = spread;
                }
            }
        }
        // split in the middle
        DistanceType split_val = (bbox[cutfeat].low+bbox[cutfeat].high)/2;
        ElementType min_elem, max_elem;
        computeMinMax(ind, count, cutfeat, min_elem, max_elem);

        if (split_val<min_elem) cutval = (DistanceType)min_elem;
        else if (split_val>max_elem) cutval = (DistanceType)max_elem;
        else cutval = split_val;

        int lim1, lim2;
        planeSplit(ind, count, cutfeat, cutval, lim1, lim2);

        if (lim1>count/2) index = lim1;
        else if (lim2<count/2) index = lim2;
        else index = count/2;

        assert(index > 0 && index < count);
    }


    /**
     *  Subdivide the list of points by a plane perpendicular on axe corresponding
     *  to the 'cutfeat' dimension at 'cutval' position.
     *
     *  On return:
     *  dataset[ind[0..lim1-1]][cutfeat]<cutval
     *  dataset[ind[lim1..lim2-1]][cutfeat]==cutval
     *  dataset[ind[lim2..count]][cutfeat]>cutval
     */
    void planeSplit(int* ind, int count, int cutfeat, DistanceType cutval, int& lim1, int& lim2)
    {
        int left = 0;
        int right = count-1;
        for (;; ) {
            while (left<=right && dataset_[ind[left]][cutfeat]<cutval) ++left;
            while (left<=right && dataset_[ind[right]][cutfeat]>=cutval) --right;
            if (left>right) break;
            std::swap(ind[left], ind[right]); ++left; --right;
        }
        lim1 = left;
        right = count-1;
        for (;; ) {
            while (left<=right && dataset_[ind[left]][cutfeat]<=cutval) ++left;
            while (left<=right && dataset_[ind[right]][cutfeat]>cutval) --right;
            if (left>right) break;
            std::swap(ind[left], ind[right]); ++left; --right;
        }
        lim2 = left;
    }

    DistanceType computeInitialDistances(const ElementType* vec, std::vector<DistanceType>& dists)
    {
        DistanceType distsq = 0.0;

        for (size_t i = 0; i < veclen_; ++i) {
            if (vec[i] < root_bbox_[i].low) {
                dists[i] = distance_.accum_dist(vec[i], root_bbox_[i].low, i);
                distsq += dists[i];
            }
            if (vec[i] > root_bbox_[i].high) {
                dists[i] = distance_.accum_dist(vec[i], root_bbox_[i].high, i);
                distsq += dists[i];
            }
        }

        return distsq;
    }

    /**
     * Performs an exact search in the tree starting from a node.
     */
    template<typename ResultSet>
    void searchLevel(ResultSet& result_set, const ElementType* vec, const NodePtr node, DistanceType mindistsq,
                     std::vector<DistanceType>& dists, const float epsError)
    {
        /* If this is a leaf node, then do check and return. */
        if ((node->child1 == NULL)&&(node->child2 == NULL)) {
            DistanceType worst_dist = result_set.worstDist();
            for (int i=node->left; i<node->right; ++i) {
                int index = reorder_ ? i : vind_[i];
                if (removed_points_.test(index)) continue;
                DistanceType dist = distance_(vec, data_[index], veclen_, worst_dist);
                if (dist<worst_dist) {
                    result_set.addPoint(dist,vind_[i]);
                }
            }
            return;
        }

        /* Which child branch should be taken first? */
        int idx = node->divfeat;
        ElementType val = vec[idx];
        DistanceType diff1 = val - node->divlow;
        DistanceType diff2 = val - node->divhigh;

        NodePtr bestChild;
        NodePtr otherChild;
        DistanceType cut_dist;
        if ((diff1+diff2)<0) {
            bestChild = node->child1;
            otherChild = node->child2;
            cut_dist = distance_.accum_dist(val, node->divhigh, idx);
        }
        else {
            bestChild = node->child2;
            otherChild = node->child1;
            cut_dist = distance_.accum_dist( val, node->divlow, idx);
        }

        /* Call recursively to search next level down. */
        searchLevel(result_set, vec, bestChild, mindistsq, dists, epsError);

        DistanceType dst = dists[idx];
        mindistsq = mindistsq + cut_dist - dst;
        dists[idx] = cut_dist;
        if (mindistsq*epsError<=result_set.worstDist()) {
            searchLevel(result_set, vec, otherChild, mindistsq, dists, epsError);
        }
        dists[idx] = dst;
    }

private:

    int leaf_max_size_;
    bool reorder_;

    /**
     *  Array of indices to vectors in the dataset.
     */
    std::vector<int> vind_;

    Matrix<ElementType> data_;

    /**
     * Array of k-d trees used to find neighbours.
     */
    NodePtr root_node_;

    /**
     * Root bounding box
     */
    BoundingBox root_bbox_;

    /**
     * Pooled memory allocator.
     *
     * Using a pooled memory allocator is more efficient
     * than allocating memory directly when there is a large
     * number small of memory allocations.
     */
    PooledAllocator pool_;

    Distance distance_;

    using BaseClass::removed_points_;
    using BaseClass::dataset_;
    using BaseClass::ownDataset_;
    using BaseClass::size_;
    using BaseClass::veclen_;
    using BaseClass::index_params_;
    using BaseClass::extendDataset;
    using BaseClass::setDataset;
};   // class KDTreeSingleIndex

}

#endif //FLANN_KDTREE_SINGLE_INDEX_H_
