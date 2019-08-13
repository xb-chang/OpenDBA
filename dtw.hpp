#ifndef __dtw_hpp_included
#define __dtw_hpp_included

#include "cuda_utils.hpp"
#include "limits.hpp" // for device side numeric_limits min() and max()

using namespace cudahack; // for device side numeric_limits

__device__ __constant__ unsigned char NIL = 255; // sentinel value for the start of the DTW alignment, the stop condition for backtracking (ergo has no corresponding moveI or moveJ)
__device__ __constant__ unsigned char DIAGONAL = 0;
__device__ __constant__ unsigned char RIGHT = 1;
__device__ __constant__ unsigned char UP = 2;
// Special move designations that do not affect backtracking algorithm per se, but does affect cost (open=no accumulation of cost for rightward move). 
__device__ __constant__ unsigned char OPEN_RIGHT = 3; 
__device__ __constant__ unsigned char NIL_OPEN_RIGHT = 254; 

// For two series I & J, encode that the cost matrix DTW path (i,j) backtracking index decrement options for the DTW steps declared above are:
// DIAGONAL => (-1,-1), RIGHT => (0,-1), UP => (-1,0), OPEN_RIGHT => (0,-1)
__device__ __constant__ short moveI[] = { -1, 0, -1, 0 };
__device__ __constant__ short moveJ[] = { -1, -1, 0, -1 };

// How to find the 1D index of (X,Y) in the pitched (i.e. coalescing memory access aligned) memory for the DTW path matrix
// Doing it column major (to mentally match the dtwCostSoFar vertical swath 1D right edge indices), whereas convention is row major in most CUDA code.
#define pitchedCoord(Column,Row,mem_pitch) ((size_t) ((Row)*(mem_pitch))+(Column))

#define ARITH_SERIES_SUM(n) (((n)*(n+1))/2)

// Need this because you cannot template dynamically allocated kernel memory in CUDA, as per https://stackoverflow.com/questions/27570552/templated-cuda-kernel-with-dynamic-shared-memory
template <typename T>
__device__ T* shared_memory_proxy()
{
    // do we need an __align__() here? I don't think so...
    extern __shared__ unsigned char memory[];
    return reinterpret_cast<T*>(memory);
}

/**
 * Compute the distance between a given pair of sequences along every White-Neely step pattern option, for the given vertical swath of the cost matrix.
 */
template<typename T>
__global__ void DTWDistance(T *second_seq_input, size_t second_seq_input_length, size_t first_seq_index, size_t offset_within_second_seq, T *gpu_sequences, size_t maxSeqLength, size_t num_sequences, size_t *gpu_sequence_lengths, 
                       T *dtwCostSoFar, unsigned char *pathMatrix, size_t pathMemPitch, T *dtwPairwiseDistances, int use_open_start, int use_open_end){
	// We need temporary storage for three rows of the cost matrix to calculate the optimal path steps as a diagonal "wavefront" until we iterate 
	// through every position of the first sequence.
	T *costs = shared_memory_proxy<T>();

	// Which two are we comparing in this threadblock?
	//int second_seq_index = second_seq_input ? 0 : first_seq_index+blockIdx.x+1;
	
	// See if there is anything to process in this thread block 
	size_t second_seq_length = second_seq_input ? second_seq_input_length : gpu_sequence_lengths[first_seq_index+blockIdx.x+1];
	if(offset_within_second_seq >= second_seq_length){
		return; // all threads in the threadblock will return
	}

	// Point to the correct spot in global memory where the costs are being stored.
	dtwCostSoFar = &dtwCostSoFar[gpu_sequence_lengths[first_seq_index]*blockIdx.x];

	size_t first_seq_length = gpu_sequence_lengths[first_seq_index];
	T *first_seq = &gpu_sequences[first_seq_index*maxSeqLength];
	T *second_seq = second_seq_input ? second_seq_input : &gpu_sequences[(first_seq_index+blockIdx.x+1)*maxSeqLength];

	// Each thread will be using the same second sequence value throughout the rest of the kernel, so store it as a local variable for efficiency.
	T second_seq_thread_val = offset_within_second_seq+threadIdx.x >= second_seq_length ? 0 : second_seq[offset_within_second_seq+threadIdx.x];

	if(threadIdx.x == 0){
		// Populate the bottom row of the vertical swath on every kernel invocation, this can't be done in parallel.
		T first_seq_start_val = first_seq[0];
		if(offset_within_second_seq == 0){
			costs[0] = 0;
			if(pathMatrix != 0){
				pathMatrix[pitchedCoord(0,0,pathMemPitch)] = use_open_start ? NIL_OPEN_RIGHT : NIL; // sentinel for path backtracking algorithm termination
			}
		}
		else{
			costs[0] = dtwCostSoFar[0];
			if(pathMatrix != 0){
				pathMatrix[pitchedCoord(offset_within_second_seq,0,pathMemPitch)] = use_open_start ? OPEN_RIGHT : RIGHT;
			}
		}
		costs[0] += use_open_start ? 0 : (first_seq_start_val-second_seq_thread_val)*(first_seq_start_val-second_seq_thread_val);
		int i;
		for(i = 1; i < blockDim.x && offset_within_second_seq+i < second_seq_length; i++){
			T diff = use_open_start ? 0 : first_seq_start_val-second_seq[offset_within_second_seq+i];
			costs[i+blockDim.x*(i%3)] = costs[(i-1)+blockDim.x*((i-1)%3)]+diff*diff;
			if(pathMatrix != 0){
				pathMatrix[pitchedCoord(offset_within_second_seq+i,0,pathMemPitch)] = use_open_start ? OPEN_RIGHT : RIGHT;
			}

		}
		dtwCostSoFar[0] = costs[(i-1)+blockDim.x*((i-1)%3)];
	}

	for(int i = 1; i < first_seq_length+blockDim.x; i++){

		if(offset_within_second_seq+threadIdx.x < second_seq_length && // We're within the sequence bounds?
		   threadIdx.x < i &&                                          // The diff still corresponds to a spot in the cost matrix?
		   i-threadIdx.x < first_seq_length){
			T up_cost = numeric_limits<T>::max();
			T diag_cost = numeric_limits<T>::max();
			T right_cost = numeric_limits<T>::max();
			T diff = first_seq[i-threadIdx.x]-second_seq_thread_val;

			// The left edge of cost matrix vertical swath is a special case as we need to 
			// access previously global mem stored intermediate costs.
			if(threadIdx.x == 0){
				// Straight up is always an option
				up_cost = costs[blockDim.x*((i-1)%3)] + diff*diff;
				if(offset_within_second_seq != 0){
					// All three steps are possible, two drawn from previous intermediate results
					right_cost = dtwCostSoFar[i] + diff*diff;
					diag_cost = dtwCostSoFar[i-1] + diff*diff;
				}
			}
			// For all other threads all the input data is stored locally in costs[].
			else{
				up_cost = costs[threadIdx.x+blockDim.x*((i-1)%3)] + diff*diff;
				right_cost = costs[(threadIdx.x-1)+blockDim.x*((i-1)%3)] + diff*diff;
				diag_cost = costs[(threadIdx.x-1)+blockDim.x*((i-2)%3)] + diff*diff;
			}

			// Use the White-Neely step pattern (a diagonal move is preferred to right-up or up-right if costs are equivalent).
			if(use_open_end && i-threadIdx.x == first_seq_length-1){
				// No extra cost to consume a sequence element from the first sequence, just copy it over from the previous column.
				costs[threadIdx.x+blockDim.x*(i%3)] = costs[threadIdx.x+blockDim.x*((i-1)%3)];
				if(pathMatrix != 0){
					// Path coordinates are global, not threadblock local like the dtwCostSoFar.
					pathMatrix[pitchedCoord(offset_within_second_seq+threadIdx.x,i-threadIdx.x,pathMemPitch)] = OPEN_RIGHT;
				}
			}
			else if(diag_cost > up_cost){
				if(up_cost > right_cost){
					costs[threadIdx.x+blockDim.x*(i%3)] = right_cost;
					if(pathMatrix != 0){pathMatrix[pitchedCoord(offset_within_second_seq+threadIdx.x,i-threadIdx.x,pathMemPitch)] = RIGHT;}
				}
				else{
					costs[threadIdx.x+blockDim.x*(i%3)] = up_cost;
					if(pathMatrix != 0){pathMatrix[pitchedCoord(offset_within_second_seq+threadIdx.x,i-threadIdx.x,pathMemPitch)] = UP;}
				}
			}
			else{
				if(diag_cost > right_cost){
					costs[threadIdx.x+blockDim.x*(i%3)] = right_cost;
                                        if(pathMatrix != 0){pathMatrix[pitchedCoord(offset_within_second_seq+threadIdx.x,i-threadIdx.x,pathMemPitch)] = RIGHT;}
				}
				else{
					costs[threadIdx.x+blockDim.x*(i%3)] = diag_cost;
					if(pathMatrix != 0){pathMatrix[pitchedCoord(offset_within_second_seq+threadIdx.x,i-threadIdx.x,pathMemPitch)] = DIAGONAL;}
				}
			}

			// Right edge is a special case as we need to store back out intermediate result to global mem
			// for the use of the next kernel call with a larger offset_within_second_seq.
			if(threadIdx.x == blockDim.x-1 || offset_within_second_seq+threadIdx.x == second_seq_length - 1){
				dtwCostSoFar[i-threadIdx.x] = costs[threadIdx.x+blockDim.x*(i%3)];
			}
		}

		// To ensure all required previous costs from neighbouring threads are calculated and available for the next iteration.
		__syncthreads();
	}

	// If this is the end of the second sequence, we now know the total cost of the alignment and can populate 
	// global var dtwPairwiseDistances.
	if(offset_within_second_seq+blockDim.x >= second_seq_length){
		if(dtwPairwiseDistances != 0 && threadIdx.x == 0){
			// 1D index for row into distances upper left pairs triangle is the total size of the triangle, minus all those that haven't been processed yet. 
			dtwPairwiseDistances[ARITH_SERIES_SUM(num_sequences-1)-ARITH_SERIES_SUM(num_sequences-first_seq_index-1)+blockIdx.x] = (T) sqrtf(dtwCostSoFar[first_seq_length-1]);
		}
	}

}

#endif