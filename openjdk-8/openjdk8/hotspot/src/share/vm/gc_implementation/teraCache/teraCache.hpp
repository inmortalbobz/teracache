#ifndef SHARE_VM_GC_IMPLEMENTATION_TERACACHE_TERACACHE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_TERACACHE_TERACACHE_HPP
#include "gc_implementation/parallelScavenge/objectStartArray.hpp"
#include "gc_interface/collectedHeap.inline.hpp"
#include "oops/oop.hpp"
#include "utilities/globalDefinitions.hpp"
#include <regions.h>
#include <thpool.h>
#include <map>
#include <tr1/tuple>

class TeraCache {
	private:
		static char*    _start_addr;          // TeraCache start address of mmap region
		static char*    _stop_addr;           // TeraCache ends address of mmap region

		static ObjectStartArray _start_array; // Keeps track of where objects 
											  // start in a 512b block
											  
		/*-----------------------------------------------
		 * Stacks
		 *---------------------------------------------*/
		// Stack to keep back pointers (Objects that are pointed out of
		// TeraCache objects) to mark them as alive durin mark_and_push phase of
		// the Full GC.
		static Stack<oop *, mtGC> _tc_stack;

		// Stack to keep the element addresses of objects that are located in
		// TeraCache and point to objects in the heap. We adjust these pointers
		// during adjust phase of the Full GC.
		static Stack<oop *, mtGC> _tc_adjust_stack;

		/*-----------------------------------------------
		 * Statistics of TeraCache
		 *---------------------------------------------*/
		static uint64_t total_active_regions;      //< Number of active regions
		static uint64_t total_merged_regions;      //< Number of merged regions

		static uint64_t total_objects;             //< Total number of objects located in TeraCache
		static uint64_t total_objects_size;        //< Total number of objects size

		static uint64_t fwd_ptrs_per_fgc;	       //< Total number of forward ptrs per FGC
		static uint64_t back_ptrs_per_fgc;	       //< Total number of back ptrs per FGC
		static uint64_t trans_per_fgc;	           //< Total number of objects transfered to 
												   //< TeraCache per FGC
		static uint64_t tc_ct_trav_time[16];	   //< Time to traverse TeraCards card table
		static uint64_t heap_ct_trav_time[16];	   //< Time to traverse heap card tables

		static uint64_t back_ptrs_per_mgc;		   //< Total number of back ptrs per MGC

		static uint64_t obj_distr_size[3];         //< Object size distribution between B, KB, MB

		static long int cur_obj_group_id;	       //<We save the current object
   												   // group id for tera-marked
												   // object to promote this id
												   // to their reference objects
		static long int cur_obj_part_id;	       //<We save the current object
   												   // partition id for tera-marked
												   // object to promote this id
												   // to their reference objects

		HeapWord* obj_h1_addr;				       // We need to check this
												   // object that will be moved
												   // to H2 if it has back ptrs
												   // to H1

		HeapWord* obj_h2_addr;				       // We need to check this
												   // object that will be moved
												   // to H2 if it has back ptrs
												   // to H1
#if BACK_REF_STAT
		// This histogram keeps internally statistics for the backward
		// references (H2 to H1)
		std::map<oop *, std::tr1::tuple<int, int, int> > histogram;
		oop *back_ref_obj;
#endif

#if FWD_REF_STAT
		// This histogram keeps internally statistics for the forward references
		// (H1 to H2) per object
		std::map<oop, int> fwd_ref_histo;
#endif
		
#if PREFETCHING
		threadpool thpool[8];
#endif

	public:
		// Constructor
		TeraCache(); 
		
		// Close TeraCache and unmap all the pages
		void tc_shutdown(); 

		// Check if this object is located in TeraCache
		bool tc_check(oop ptr);
		
		// Check if object p belongs to TeraCache
		bool tc_is_in(void* p);

		bool tc_empty(void);

		// Get region allocation address
		char* tc_get_addr_region(void);
		
		// Get region stop address
		char* tc_stop_addr_region(void);

		// Get the size of TeraCache
		size_t tc_get_size_region(void);

		// Add new object in the region
		char* tc_region_top(oop obj, size_t size);

		// Get the last access pointer of the region
		char* tc_region_cur_ptr(void);

		// Use in full GC
		void scavenge();
		
		// Increase forward ptrs from JVM heap to TeraCache
		void tc_increase_forward_ptrs();

		ObjectStartArray* start_array() { return &_start_array; }

		void tc_push_object(void *p, oop o);

		void tc_adjust();

		// Deallocate the stacks
		void tc_clear_stacks();

		// Init the statistics counters of TeraCache to zero when a Full GC
		// starts
		void tc_init_counters();

		// Print the statistics of TeraCache at the end of each FGC
		// Will print:
		//	- the total forward pointers from the JVM heap to the
		// TeraCache
		//	- the total back pointers from TeraCache to the JVM heap
		//	- the total objects that has been transfered to the TeraCache
		//	- the current total size of objects in TeraCache
		//	- the current total objects that are located in TeraCache
		void tc_print_statistics();

		// Keep for each thread with 'tid' the 'total time' that needed to
		// traverse the TeraCache card table.
		// Each thread writes the time in a table based on each ID and then we
		// take the maximum time from all the threads as the total time.
		void tc_ct_traversal_time(unsigned int tid, uint64_t total_time);
		
		// Keep for each thread with 'tid' the 'total time' that needed to
		// traverse the Heap card table.
		// Each thread writes the time in a table based on each ID and then we
		// take the maximum time from all the threads as the total time.
		void heap_ct_traversal_time(unsigned int tid, uint64_t total_time);

		// Print the statistics of TeraCache at the end of each minorGC
		// Will print:
		//	- the time to traverse the TeraCache dirty card tables
		//	- the time to traverse the Heap dirty card tables
		//	- TODO number of dirty cards in TeraCache
		//	- TODO number of dirty cards in Heap
		void tc_print_mgc_statistics();

		// Give advise to kernel to expect page references in sequential order
		void tc_enable_seq();

		// Give advise to kernel to expect page references in random order
		void tc_enable_rand();
		
		// Explicit (using systemcall) write 'data' with 'size' to the specific
		// 'offset' in the file.
		void tc_write(char *data, char *offset, size_t size);
		
		// Explicit (using systemcall) asynchronous write 'data' with 'size' to
		// the specific 'offset' in the file.
		void tc_awrite(char *data, char *offset, size_t size);

		// We need to ensure that all the writes in TeraCache using asynchronous
		// I/O have been completed succesfully.
		int tc_areq_completed();
		
		// Fsync writes in TeraCache
		// We need to make an fsync when we use fastmap
		void tc_fsync();

#if PR_BUFFER
		// Add an object 'obj' with size 'size' to the promotion buffer. 'New_adr' is
		// used to know where the object will move to H2. We use promotion buffer to
		// reduce the number of system calls for small sized objects.
		void tc_buffer_insert(char* obj, char* new_adr, size_t size);
		
		// At the end of the major GC flush and free all the promotion buffers.
		void tc_free_all_buffers();
#endif

        // Check if the object that the card is trying to reference is
        // valid. 
        bool check_if_valid_object(HeapWord *obj);
        
        // Get the ending address of the last object of the region obj
        // belongs to.
        HeapWord* get_last_object_end(HeapWord *obj);
        
        // Checks if the address of obj is the beginning of a region
        bool is_start_of_region(HeapWord *obj);

        // Resets the used field of all regions
        void reset_used_field(void);

        // Marks the region containing obj as used
        void mark_used_region(HeapWord *obj);

        // Prints all active regions
        void print_active_regions(void);
        
        // Groups the region of obj with the previously enabled region
        void group_region_enabled(HeapWord* obj, void *obj_field);

        // Frees all unused regions
        void free_unused_regions(void);
    
        // Prints all the region groups
        void print_region_groups(void);

        // Enables groupping with region of obj
        void enable_groups(HeapWord* old_addr, HeapWord* new_addr);

        // Disables region groupping
        void disable_groups(void);
        
        void print_object_name(HeapWord *obj,const char *name);

        // Groups the region of obj1 with the region of obj2
        void group_regions(HeapWord *obj1, HeapWord *obj2);
  
        HeapWord *get_first_object_in_region(HeapWord *addr);

		// We save the current object group 'id' for tera-marked object to
		// promote this 'id' to its reference objects
		void set_cur_obj_group_id(long int id);
		
		// Get the saved current object group id 
		long int get_cur_obj_group_id(void);
		
		// We save the current object partition 'id' for tera-marked object to
		// promote this 'id' to its reference objects
		void set_cur_obj_part_id(long int id);
		
		// Get the saved current object partition id 
		long int get_cur_obj_part_id(void);

		// Iterate over all objects in each region and print their states
		// This function is for debugging purposes to understand and fix the
		// locality in regions
		void tc_print_objects_per_region(void);

        void mark_live(HeapWord *p);

        void tc_mark_live_objects_per_region();
		
        void tc_count_marked_objects();

        void tc_reset_marked_objects();

		// Check if backward adjust stack is empty
		bool tc_is_empty_back_stack();

#if PREFETCHING
		void tc_prefetch_data(HeapWord *obj, long rdd_id, long part_id);
		
		void tc_wait();
#endif

		// Get the group Id of the objects that belongs to this region. We
		// locate the objects of the same group to the same region. We use the
		// field 'p' of the object to identify in which region the object
		// belongs to.
		uint64_t tc_get_region_groupId(void* p);
		
		// Get the partition Id of the objects that belongs to this region. We
		// locate the objects of the same group to the same region. We use the
		// field 'p' of the object to identify in which region the object
		// belongs to.
		uint64_t tc_get_region_partId(void* p);

#if BACK_REF_STAT
		// Add a new entry to the histogram for back reference that start from
		// 'obj' and results in H1 (new or old generation).
		void tc_add_back_ref_stat(bool is_old, bool is_tera_cache);

		void tc_enable_back_ref_traversal(oop* obj);
		
		// Print the histogram
		void tc_print_back_ref_stat();
#endif

#if FWD_REF_STAT
		// Add a new entry to the histogram for forward reference that start from
		// H1 and results in 'obj' in H2 
		void tc_add_fwd_ref_stat(oop obj);
		
		// Print the histogram
		void tc_print_fwd_ref_stat();
#endif
};

#endif
