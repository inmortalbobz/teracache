#include <iostream>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "gc_implementation/teraCache/teraCache.hpp"
#include "gc_implementation/parallelScavenge/psVirtualspace.hpp"
#include "memory/cardTableModRefBS.hpp"
#include "memory/memRegion.hpp"
#include "memory/sharedDefines.h"
#include "runtime/globals.hpp"
#include "runtime/mutexLocker.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/globalDefinitions.hpp"

char*        TeraCache::_start_addr = NULL;
char*        TeraCache::_stop_addr = NULL;

ObjectStartArray TeraCache::_start_array;
Stack<oop *, mtGC> TeraCache::_tc_stack;
Stack<oop *, mtGC> TeraCache::_tc_adjust_stack;

uint64_t TeraCache::total_active_regions;
uint64_t TeraCache::total_merged_regions;
uint64_t TeraCache::total_objects;
uint64_t TeraCache::total_objects_size;
uint64_t TeraCache::fwd_ptrs_per_fgc;
uint64_t TeraCache::back_ptrs_per_fgc;
uint64_t TeraCache::trans_per_fgc;

uint64_t TeraCache::tc_ct_trav_time[16];
uint64_t TeraCache::heap_ct_trav_time[16];
		
uint64_t TeraCache::back_ptrs_per_mgc;

uint64_t TeraCache::obj_distr_size[3];	
long int TeraCache::cur_obj_group_id;
long int TeraCache::cur_obj_part_id;

// Constructor of TeraCache
TeraCache::TeraCache() {
	
	uint64_t align = CardTableModRefBS::tc_ct_max_alignment_constraint();
	init(align);

	_start_addr = start_addr_mem_pool();
	_stop_addr  = stop_addr_mem_pool();

	// Initilize counters for TeraCache
	// These counters are used for experiments
	total_active_regions = 0;
	total_merged_regions = 0;

	total_objects = 0;
	total_objects_size = 0;

	// Initialize arrays for the next minor collection
	for (unsigned int i = 0; i < ParallelGCThreads; i++) {
		tc_ct_trav_time[i] = 0;
		heap_ct_trav_time[i] = 0;
	}

	back_ptrs_per_mgc = 0;

	for (unsigned int i = 0; i < 3; i++) {
		obj_distr_size[i] = 0;
	}

	cur_obj_group_id = 0;

#if PREFETCHING
	// 8 thread pools
	// wait se kathe thesi tou pinaka
	for (int i = 0; i < 8; i++) 
		thpool[i] = thpool_init(4);
#endif

	obj_h1_addr = NULL;
	obj_h2_addr = NULL;
}
		
void TeraCache::tc_shutdown() {
	r_shutdown();
}


// Check if an object `ptr` belongs to the TeraCache. If the object belongs
// then the function returns true, either it returns false.
bool TeraCache::tc_check(oop ptr) {
	return ((HeapWord *)ptr >= (HeapWord *) _start_addr)     // if greater than start address
			&& ((HeapWord *) ptr < (HeapWord *)_stop_addr);  // if smaller than stop address
}

// Check if an object `p` belongs to TeraCache. If the object bolongs to
// TeraCache then the function returns true, either it returns false.
bool TeraCache::tc_is_in(void *p) {
	char* const cp = (char *)p;
	return cp >= _start_addr && cp < _stop_addr;
}

// Return the start address of the region
char* TeraCache::tc_get_addr_region(void) {
	assertf((char *)(_start_addr) != NULL, "Region is not allocated");

	return _start_addr;
}

// Return the end address of the region
char* TeraCache::tc_stop_addr_region(void) {
	assertf((char *)(_start_addr) != NULL, "Region is not allocated");
	assertf((char *)(_stop_addr) != NULL, "Region is not allocated");

	return _stop_addr;
}

// Get the size of TeraCache
size_t TeraCache::tc_get_size_region(void) {
	return mem_pool_size();
}

// Allocate new object 'obj' with 'size' in words in TeraCache.
// Return the allocated 'pos' position of the object
char* TeraCache::tc_region_top(oop obj, size_t size) {
	char *pos;			// Allocation position

	// Update Statistics
	total_objects_size += size;
	total_objects++;
	trans_per_fgc++;

	if (TeraCacheStatistics) {
		size_t obj_size = (size * HeapWordSize) / 1024;
		int count = 0;

		while (obj_size > 0) {
			count++;
			obj_size/=1024;
		}

		assertf(count <=2, "Array out of range");

		obj_distr_size[count]++;
	}

	pos = allocate(size,(uint64_t)obj->get_obj_group_id(),(uint64_t)obj->get_obj_part_id());

#if VERBOSE_TC
	if (TeraCacheStatistics)
		tclog_or_tty->print_cr("[STATISTICS] | OBJECT: %p | SIZE = %lu | ID = %d | NAME = %s",
				pos, size, obj->get_obj_group_id(), obj->klass()->internal_name());
#endif

	_start_array.tc_allocate_block((HeapWord *)pos);

	return pos;
}

// Get the allocation top pointer of the TeraCache
char* TeraCache::tc_region_cur_ptr(void) {
	return cur_alloc_ptr();
}

// Pop the objects that are in `_tc_stack` and mark them as live object. These
// objects are located in the Java Heap and we need to ensure that they will be
// kept alive.
void TeraCache::scavenge()
{
	struct timeval start_time;
	struct timeval end_time;

	gettimeofday(&start_time, NULL);

	while (!_tc_stack.is_empty()) {
		oop* obj = _tc_stack.pop();

		if (TeraCacheStatistics)
			back_ptrs_per_fgc++;

#if P_SD_BACK_REF_CLOSURE && !SPARK_POLICY
		MarkSweep::tera_back_ref_mark_and_push(obj);
#else
		MarkSweep::mark_and_push(obj);
#endif
	}
	
	gettimeofday(&end_time, NULL);

	if (TeraCacheStatistics)
		tclog_or_tty->print_cr("[STATISTICS] | TC_MARK = %llu\n", 
				(unsigned long long)((end_time.tv_sec - start_time.tv_sec) * 1000) + // convert to ms
				(unsigned long long)((end_time.tv_usec - start_time.tv_usec) / 1000)); // convert to ms
}
		
void TeraCache::tc_push_object(void *p, oop o) {
	MutexLocker x(tera_cache_lock);
	_tc_stack.push((oop *)p);
	_tc_adjust_stack.push((oop *)p);
	
	back_ptrs_per_mgc++;

	assertf(!_tc_stack.is_empty(), "Sanity Check");
	assertf(!_tc_adjust_stack.is_empty(), "Sanity Check");
}

// Adjust backwards pointers during Full GC.  
void TeraCache::tc_adjust() {
	struct timeval start_time;
	struct timeval end_time;

	gettimeofday(&start_time, NULL);

	while (!_tc_adjust_stack.is_empty()) {
		oop* obj = _tc_adjust_stack.pop();
        Universe::teraCache()->enable_groups(NULL, (HeapWord*) obj);
		MarkSweep::adjust_pointer(obj);
        Universe::teraCache()->disable_groups();
	}
	
	gettimeofday(&end_time, NULL);

	if (TeraCacheStatistics)
		tclog_or_tty->print_cr("[STATISTICS] | TC_ADJUST %llu\n",
				(unsigned long long)((end_time.tv_sec - start_time.tv_sec) * 1000) + // convert to ms
				(unsigned long long)((end_time.tv_usec - start_time.tv_usec) / 1000)); // convert to ms
}

// Increase the number of forward ptrs from JVM heap to TeraCache
void TeraCache::tc_increase_forward_ptrs() {
	fwd_ptrs_per_fgc++;
}

// Check if the TeraCache is empty. If yes, return 'true', 'false' otherwise
bool TeraCache::tc_empty() {
	return r_is_empty();
}
		
void TeraCache::tc_clear_stacks() {
	if (TeraCacheStatistics)
		back_ptrs_per_mgc = 0;
		
	_tc_adjust_stack.clear(true);
	_tc_stack.clear(true);
}

// Check if backward adjust stack is empty
bool TeraCache::tc_is_empty_back_stack() {
	return _tc_adjust_stack.is_empty();
}

// Init the statistics counters of TeraCache to zero when a Full GC
// starts
void TeraCache::tc_init_counters() {
	fwd_ptrs_per_fgc  = 0;	
	back_ptrs_per_fgc = 0;
	trans_per_fgc     = 0;
}

// Print the statistics of TeraCache at the end of each FGC
// Will print:
//	- the total forward pointers from the JVM heap to the TeraCache
//	- the total back pointers from TeraCache to the JVM heap
//	- the total objects that has been transfered to the TeraCache
//	- the current total size of objects in TeraCache until
//	- the current total objects that are located in TeraCache
void TeraCache::tc_print_statistics() {
	tclog_or_tty->print_cr("[STATISTICS] | TOTAL_FORWARD_PTRS = %lu\n", fwd_ptrs_per_fgc);
	tclog_or_tty->print_cr("[STATISTICS] | TOTAL_BACK_PTRS = %lu\n", back_ptrs_per_fgc);
	tclog_or_tty->print_cr("[STATISTICS] | TOTAL_TRANS_OBJ = %lu\n", trans_per_fgc);

	tclog_or_tty->print_cr("[STATISTICS] | TOTAL_OBJECTS  = %lu\n", total_objects);
	tclog_or_tty->print_cr("[STATISTICS] | TOTAL_OBJECTS_SIZE = %lu\n", total_objects_size);
	tclog_or_tty->print_cr("[STATISTICS] | DISTRIBUTION | B = %lu | KB = %lu | MB = %lu\n",
			obj_distr_size[0], obj_distr_size[1], obj_distr_size[2]);

#if FWD_REF_STAT
	tc_print_fwd_ref_stat();
#endif
}
		
// Keep for each thread the time that need to traverse the TeraCache
// card table.
// Each thread writes the time in a table based on each ID and then we
// take the maximum time from all the threads as the total time.
void TeraCache::tc_ct_traversal_time(unsigned int tid, uint64_t total_time) {
	if (tc_ct_trav_time[tid]  < total_time)
		tc_ct_trav_time[tid] = total_time;
}

// Keep for each thread the time that need to traverse the Heap
// card table
// Each thread writes the time in a table based on each ID and then we
// take the maximum time from all the threads as the total time.
void TeraCache::heap_ct_traversal_time(unsigned int tid, uint64_t total_time) {
	if (heap_ct_trav_time[tid]  < total_time)
		heap_ct_trav_time[tid] = total_time;
}

// Print the statistics of TeraCache at the end of each minorGC
// Will print:
//	- the time to traverse the TeraCache dirty card tables
//	- the time to traverse the Heap dirty card tables
//	- TODO number of dirty cards in TeraCache
//	- TODO number of dirty cards in Heap
void TeraCache::tc_print_mgc_statistics() {
	uint64_t max_tc_ct_trav_time = 0;		//< Maximum traversal time of
											// TeraCache card tables from all
											// threads
	uint64_t max_heap_ct_trav_time = 0;     //< Maximum traversal time of Heap
											// card tables from all the threads

	for (unsigned int i = 0; i < ParallelGCThreads; i++) {
		if (max_tc_ct_trav_time < tc_ct_trav_time[i])
			max_tc_ct_trav_time = tc_ct_trav_time[i];
		
		if (max_heap_ct_trav_time < heap_ct_trav_time[i])
			max_heap_ct_trav_time = heap_ct_trav_time[i];
	}

	tclog_or_tty->print_cr("[STATISTICS] | TC_CT_TIME = %lu\n", max_tc_ct_trav_time);
	tclog_or_tty->print_cr("[STATISTICS] | HEAP_CT_TIME = %lu\n", max_heap_ct_trav_time);
	tclog_or_tty->print_cr("[STATISTICS] | BACK_PTRS_PER_MGC = %lu\n", back_ptrs_per_mgc);

#if BACK_REF_STAT
	tc_print_back_ref_stat();
#endif
	
	// Initialize arrays for the next minor collection
	for (unsigned int i = 0; i < ParallelGCThreads; i++) {
		tc_ct_trav_time[i] = 0;
		heap_ct_trav_time[i] = 0;
	}

	// Initialize counters
	back_ptrs_per_mgc = 0;
}

// Give advise to kernel to expect page references in sequential order
void TeraCache::tc_enable_seq() {
#if FMAP_HYBRID
	r_enable_huge_flts();
#elif MADVISE_ON
	r_enable_seq();
#endif
}

// Give advise to kernel to expect page references in random order
void TeraCache::tc_enable_rand() {
#if FMAP_HYBRID
	r_enable_regular_flts();
#elif MADVISE_ON
	r_enable_rand();
#endif
}
		
// Explicit (using systemcall) write 'data' with 'size' to the specific
// 'offset' in the file.
void TeraCache::tc_write(char *data, char *offset, size_t size) {
	r_write(data, offset, size);
}

// Explicit (using systemcall) asynchronous write 'data' with 'size' to
// the specific 'offset' in the file.
void TeraCache::tc_awrite(char *data, char *offset, size_t size) {
	r_awrite(data, offset, size);
}
		
// We need to ensure that all the writes in TeraCache using asynchronous
// I/O have been completed succesfully.
int TeraCache::tc_areq_completed() {
	return r_areq_completed();

}
		
// Fsync writes in TeraCache
// We need to make an fsync when we use fastmap
void TeraCache::tc_fsync() {
	r_fsync();
}

// Checks if the address of obj is before the empty part of the region
bool TeraCache::check_if_valid_object(HeapWord *obj) {
    return is_before_last_object((char *)obj);
}

// Returns the ending address of the last object in the region obj
// belongs to
HeapWord* TeraCache::get_last_object_end(HeapWord *obj) {
    return (HeapWord*)get_last_object((char *) obj);
}

// Checks if the address of obj is the beginning of a region
bool TeraCache::is_start_of_region(HeapWord *obj) {
    return is_region_start((char *) obj);
}

// Resets the used field of all regions
void TeraCache::reset_used_field(void) {
    reset_used();
}

// Marks the region containing obj as used
void TeraCache::mark_used_region(HeapWord *obj) {
    mark_used((char *) obj);
}

// Prints all active regions
void TeraCache::print_active_regions(void){
    print_used_regions();
}

// If obj is in a different tc region than the region enabled, they
// are grouped 
void TeraCache::group_region_enabled(HeapWord* obj, void *obj_field) {
	// Object is not going to be moved to TeraCache
	if (obj_h2_addr == NULL) 
		return;

	if (tc_check(oop(obj))) {
		check_for_group((char*) obj);
		return;
	}

	// If it is an already backward pointer popped from tc_adjust_stack then do
	// not mark the card as dirty because it is already marked from minor gc.
	if (obj_h1_addr == NULL) 
		return;
	
	// Mark the H2 card table as dirty if obj is in H1 (backward reference)
	BarrierSet* bs = Universe::heap()->barrier_set();

	if (bs->is_a(BarrierSet::ModRef)) {
		ModRefBarrierSet* modBS = (ModRefBarrierSet*)bs;

		size_t diff =  (HeapWord *)obj_field - obj_h1_addr;
		assertf(diff > 0 && (diff <= (uint64_t) oop(obj_h1_addr)->size()), 
				"Diff out of range: %lu", diff);
		HeapWord *h2_obj_field = obj_h2_addr + diff;
		assertf(tc_is_in((void *) h2_obj_field), "Shoud be in H2");

		modBS->tc_write_ref_field(h2_obj_field);

#if DEBUG_TERACACHE
		fprintf(stderr, "[CARD TABLE] DIFF = %lu\n", diff);
		fprintf(stderr, "[CARD TABLE] H1: Start Obj = %p | Field = %p\n",
				obj_h1_addr, obj_field);
		fprintf(stderr, "[CARD TABLE] H2: Start Obj = %p | Field = %p\n",
				obj_h2_addr, h2_obj_field);
		fprintf(stderr, "[CARD TABLE] H2: Obj = %p | SIZE = %d | NAME = %s\n",
				obj_h1_addr, oop(obj_h1_addr)->size(), 
				oop(obj_h1_addr)->klass()->internal_name());
#endif
	}
}

// Frees all unused regions
void TeraCache::free_unused_regions(void){
    struct region_list *ptr = free_regions();
    struct region_list *prev = NULL;
    while (ptr != NULL){
        start_array()->tc_region_reset((HeapWord*)ptr->start,(HeapWord*)ptr->end);
        prev = ptr;
        ptr = ptr->next;
        free(prev);
    }
}
   
// Prints all the region groups
void TeraCache::print_region_groups(void){
    print_groups();
}

// Enables groupping with region of obj
void TeraCache::enable_groups(HeapWord *old_addr, HeapWord* new_addr){
    enable_region_groups((char*) new_addr);

	obj_h1_addr = old_addr;
	obj_h2_addr = new_addr;
}

// Disables region groupping
void TeraCache::disable_groups(void){
    disable_region_groups();

	obj_h1_addr = NULL;
	obj_h2_addr = NULL;
}


// Groups the region of obj1 with the region of obj2
void TeraCache::group_regions(HeapWord *obj1, HeapWord *obj2){
	if (is_in_the_same_group((char *) obj1, (char *) obj2)) 
		return;
	MutexLocker x(tera_cache_group_lock);
    references((char*) obj1, (char*) obj2);
}

#if PR_BUFFER

// Add an object 'obj' with size 'size' to the promotion buffer. 'New_adr' is
// used to know where the object will move to H2. We use promotion buffer to
// reduce the number of system calls for small sized objects.
void  TeraCache::tc_buffer_insert(char* obj, char* new_adr, size_t size) {
	buffer_insert(obj, new_adr, size);
}

// At the end of the major GC flush and free all the promotion buffers.
void TeraCache::tc_free_all_buffers() {
	free_all_buffers();
}
#endif

// We save the current object group 'id' for tera-marked object to
// promote this 'id' to its reference objects
void TeraCache::set_cur_obj_group_id(long int id) {
	cur_obj_group_id = id;
}

// Get the saved current object group id 
long int TeraCache::get_cur_obj_group_id(void) {
	return cur_obj_group_id;
}

// We save the current object partition 'id' for tera-marked object to
// promote this 'id' to its reference objects
void TeraCache::set_cur_obj_part_id(long int id) {
	cur_obj_part_id = id;
}

// Get the saved current object partition id 
long int TeraCache::get_cur_obj_part_id(void) {
	return cur_obj_part_id;
}

void TeraCache::print_object_name(HeapWord *obj,const char *name){
    print_objects_temporary_function((char *)obj, name);
}

void TeraCache::tc_print_objects_per_region() {
	HeapWord *next_region;
	HeapWord *obj_addr;
	oop obj;

	start_iterate_regions();

	next_region = (HeapWord *) get_next_region();

	while(next_region != NULL) {
		obj_addr = next_region;

		while (1) {
			obj = oop(obj_addr);

			fprintf(stderr, "[PLACEMENT] OBJ = %p | RDD = %d | PART_ID = %lu\n", obj,
					obj->get_obj_group_id(), obj->get_obj_part_id());

			if (!check_if_valid_object(obj_addr + obj->size()))
				break;

			obj_addr += obj->size();
		}

		next_region = (HeapWord *) get_next_region();
	}
}


HeapWord *TeraCache::get_first_object_in_region(HeapWord *addr){
    return (HeapWord*) get_first_object((char*)addr);
}

void TeraCache::tc_count_marked_objects(){
	HeapWord *next_region;
	HeapWord *obj_addr;
	oop obj;

	start_iterate_regions();

	next_region = (HeapWord *) get_next_region();
    int region_num = 0;
    unsigned int live_objects = 0;
    unsigned int total_objects = 0;
	while(next_region != NULL) {
        int r_live_objects = 0;
        int r_total_objects = 0;

		obj_addr = next_region;
        
		while (1) {
			obj = oop(obj_addr);
            r_total_objects++;
            total_objects++;
            if (obj->is_live()){
                r_live_objects++;
                live_objects++;
            }
			if (!check_if_valid_object(obj_addr + obj->size()))
				break;
			obj_addr += obj->size();
		}
        printf("Region %d has %d live objects out of a total of %d\n",region_num,r_live_objects,r_total_objects);
        region_num++;
		next_region = (HeapWord *) get_next_region();
	}
    printf("GLOBAL: %d live objects out of a total of %d\n",live_objects,total_objects);
    fflush(stdout);
}

void TeraCache::tc_reset_marked_objects() {
	HeapWord *next_region;
	HeapWord *obj_addr;
	oop obj;

	start_iterate_regions();

	next_region = (HeapWord *) get_next_region();

	while(next_region != NULL) {
		obj_addr = next_region;

		while (1) {
			obj = oop(obj_addr);
            obj->reset_live();
			if (!check_if_valid_object(obj_addr + obj->size()))
				break;
			obj_addr += obj->size();
		}
		next_region = (HeapWord *) get_next_region();
	}
}

void TeraCache::tc_mark_live_objects_per_region() {
	HeapWord *next_region;
	HeapWord *obj_addr;
	oop obj;

	start_iterate_regions();

	next_region = (HeapWord *) get_next_region();
	while(next_region != NULL) {
		obj_addr = next_region;

		while (1) {
			obj = oop(obj_addr);
            if (obj->is_live()){
                obj->klass()->oop_follow_contents_tera_cache(obj,true);
            }
			if (!check_if_valid_object(obj_addr + obj->size()))
				break;
			obj_addr += obj->size();
		}
		next_region = (HeapWord *) get_next_region();
	}
    tc_count_marked_objects();
    tc_reset_marked_objects();
}

#if PREFETCHING

void print_timestamp(HeapWord* obj, bool start) {
	time_t timer;
    char buffer[26];
    struct tm* tm_info;

    timer = time(NULL);
    tm_info = localtime(&timer);

    strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

	if (start)
		fprintf(stderr, "[START PREFETCHING] OBJ = %p | TIMESTAMP = %s\n", obj, buffer);
	else
		fprintf(stderr, "[STOP PREFETCHING] OBJ = %p | TIMESTAMP = %s\n", obj, buffer);
}
/**
 * MT to prefetch data
 */
void prefetch_data(void *obj) {
	HeapWord *obj_addr = (HeapWord *) obj;

	int step = (2 * 1024 * 1024);
	volatile int sum = 0;
	volatile int *prefetch_me;

	while(1) {
		prefetch_me = (int *) obj_addr;
		sum += *prefetch_me;

		if (!is_before_last_object((char *)(obj_addr + step)))
			break;
			
		obj_addr += step;
	}

	return;
}

void TeraCache::tc_prefetch_data(HeapWord *obj, long rdd_id, long part_id) {

	HeapWord *reg_start_address = 
		(HeapWord *) get_region_start_addr((char *) obj, rdd_id, part_id);

	thpool_add_work(thpool[rdd_id % 8], prefetch_data, (void *)reg_start_address);
	return;
}

void TeraCache::tc_wait() {
	//thpool_wait(thpool);
}
#endif

// Get the group Id of the objects that belongs to this region. We
// locate the objects of the same group to the same region. We use the
// field 'p' of the object to identify in which region the object
// belongs to.
uint64_t TeraCache::tc_get_region_groupId(void* p) {
	assertf((char *) p != NULL, "Sanity check");
	return get_obj_group_id((char *) p);
}

// Get the partition Id of the objects that belongs to this region. We
// locate the objects of the same group to the same region. We use the
// field 'p' of the object to identify in which region the object
// belongs to.
uint64_t TeraCache::tc_get_region_partId(void* p) {
	assertf((char *) p != NULL, "Sanity check");
	return get_obj_part_id((char *) p);
}

#if BACK_REF_STAT
// Add a new entry to the histogram for 'obj'
void TeraCache::tc_add_back_ref_stat(bool is_old, bool is_tera_cache) {
	std::tr1::tuple<int, int, int> val;
	std::tr1::tuple<int, int, int> new_val;

	val = histogram[back_ref_obj];
	
	if (is_old) {                         // Reference is in the old generation  
		new_val = std::tr1::make_tuple(
				std::tr1::get<0>(val),
				std::tr1::get<1>(val) + 1,
				std::tr1::get<2>(val));
	}
	else if (is_tera_cache) {             // Reference is in the tera cache
		new_val = std::tr1::make_tuple(
				std::tr1::get<0>(val),
				std::tr1::get<1>(val),
				std::tr1::get<2>(val) + 1);
	} else {                              // Reference is in the new generation
		new_val = std::tr1::make_tuple(
				std::tr1::get<0>(val) + 1,
				std::tr1::get<1>(val),
				std::tr1::get<2>(val));
	}
	
	histogram[back_ref_obj] = new_val;
}
		
// Enable traversal class object
void TeraCache::tc_enable_back_ref_traversal(oop* obj) {
	std::tr1::tuple<int, int, int> val;

	val = std::tr1::make_tuple(0, 0, 0);

	back_ref_obj = obj;
	histogram[obj] = val;

}

// Print the histogram
void TeraCache::tc_print_back_ref_stat() {
	std::map<oop *, std::tr1::tuple<int, int, int> >::const_iterator it;

	
	tclog_or_tty->print_cr("Start_Back_Ref_Statistics\n");

	for(it = histogram.begin(); it != histogram.end(); ++it) {
		if (std::tr1::get<0>(it->second) > 1000 || std::tr1::get<1>(it->second) > 1000) {
			tclog_or_tty->print_cr("[HISTOGRAM] ADDR = %p | NAME = %s | NEW = %d | OLD = %d | TC = %d\n",
					it->first, oop(it->first)->klass()->internal_name(), std::tr1::get<0>(it->second),
					std::tr1::get<1>(it->second), std::tr1::get<2>(it->second));
		}
	}
	
	tclog_or_tty->print_cr("End_Back_Ref_Statistics\n");

	// Empty the histogram at the end of each minor gc
	histogram.clear();
}
#endif

#if FWD_REF_STAT
// Add a new entry to the histogram for forward reference that start from
// H1 and results in 'obj' in H2 
void TeraCache::tc_add_fwd_ref_stat(oop obj) {
	fwd_ref_histo[obj] ++;
}

// Print the histogram
void TeraCache::tc_print_fwd_ref_stat() {
	std::map<oop,int>::const_iterator it;

	tclog_or_tty->print_cr("Start_Fwd_Ref_Statistics\n");

	for(it = fwd_ref_histo.begin(); it != fwd_ref_histo.end(); ++it) {
		tclog_or_tty->print_cr("[FWD HISTOGRAM] ADDR = %p | NAME = %s | REF = %d\n",
				it->first, oop(it->first)->klass()->internal_name(), it->second);
	}
	
	tclog_or_tty->print_cr("End_Fwd_Ref_Statistics\n");

	// Empty the histogram at the end of each major gc
	fwd_ref_histo.clear();
}

#endif

