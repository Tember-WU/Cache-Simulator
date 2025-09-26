#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <vector>
#include <iomanip>
#include "sim.h"
using namespace std;

#include <bitset>
#include <algorithm>

//----------------------Function Declaration-------------------------
unsigned int int_log2(uint32_t x);
//-------------------------------------------------------------------

typedef 
struct {
   bool valid = false;
   bool dirty = false;
   int LRU = 0;
   uint32_t address = 0;
} block_params;

class STREAM_BUFFER{
   public:
      bool valid; // Each steam buffer has a valid bit
      int LRU; // Each steam buffer has a LRU bit
      vector<uint32_t> BLOCK_ADDR;  // Each stream buffer has M consecutive memory blocks

      STREAM_BUFFER () {
         valid = false;
      }

      void setup(uint32_t index, uint32_t PREF_M){
         LRU = index;
         BLOCK_ADDR.resize(PREF_M);
         for(int i=0; i<(int)PREF_M; i++){
            BLOCK_ADDR[i] = 0;
         }
      }
};

class CACHE {
   public:
      vector<vector<block_params>> BLOCK;
      CACHE* next;
      vector<STREAM_BUFFER> StreamBuffer;
      bool hasStreamBuffer;

      uint32_t sets;
      uint32_t ways;
      uint32_t blocksize;

      int num_read;
      int num_read_miss;
      int num_write;
      int num_write_miss;
      int num_write_back;
      int num_prefetch;

      void read_request(uint32_t addr);
      void write_request(uint32_t addr);  //Issue read request to next level for the missing block
      void LRU_update(uint32_t set_index, uint32_t tag_value);
      void install_block(uint32_t set_index, uint32_t tag_value); // if the victim block is dirty, issue write request to the next level
      void make_space(uint32_t set_index);
      void print_cache_content();
      void StreamBuffer_Setup(uint32_t PREF_N, uint32_t PREF_M);
      int check_StreamBuffer(uint32_t buffer_block_tag);
      void StreamBuffer_LRU_Update(uint32_t MRU_buffer_index);
      void StreamBuffer_read_request(uint32_t buffer_block_tag);
      void Prefetch_new_stream(uint32_t buffer_index, uint32_t buffer_block_tag);
      void count_num_prefetch(uint32_t buffer_index, uint32_t buffer_block_tag);
      void print_StreamBuffer_content();

      CACHE (uint32_t num_sets, uint32_t num_ways, uint32_t block_size, uint32_t PREF_N, uint32_t PREF_M)
            : sets(num_sets), ways(num_ways), blocksize(block_size)
      {
         BLOCK.resize(num_sets);
         for(int i=0; i<(int)num_sets; i++){
            BLOCK[i].resize(num_ways);
            // Initialize LRU for each block in the same set
            // i.e., BLOCK[0].LRU = 0, BLOCK[1].LRU = 1, BLOCK[2].LRU = 2 ...
            for(int j=0; j<(int)num_ways; j++){
               BLOCK[i][j].LRU = j;
               BLOCK[i][j].valid = false;
               BLOCK[i][j].dirty = false;
               BLOCK[i][j].address = 0;
            }
         }

         next = nullptr; // initialize the next level as main memory
         num_read = 0;
         num_read_miss = 0;
         num_write = 0;
         num_write_miss = 0;
         num_write_back = 0;
         num_prefetch = 0;

         if(PREF_N != 0 && PREF_M != 0){
            hasStreamBuffer = true;
            StreamBuffer.resize(PREF_N);
            for(int i=0; i<(int)PREF_N; i++){
               StreamBuffer[i].setup(i, PREF_M);
            }
         }else{
            hasStreamBuffer = false;
         }

      };
};

void CACHE::read_request(uint32_t addr){
   // Calculation based on the current cache level configuration
   uint32_t tag, index;
   unsigned int num_block_offset = int_log2(blocksize);
   unsigned int num_index_bits = int_log2(sets);
   uint32_t mask = ((1 << num_index_bits) - 1);
   index = (addr >> num_block_offset) & mask;
   tag = addr >> (num_block_offset + num_index_bits);

   bool cache_read_hit = false;
   for(uint32_t block_index=0; block_index<ways; block_index++){
      // find through each block within the specific set
      if(BLOCK[index][block_index].valid == true && 
         BLOCK[index][block_index].address == tag) {
         cache_read_hit = true;
         break;
      }
   }

   bool StreamBuffer_read_hit = false;
   int MRU_buffer_index = -1;
   if(hasStreamBuffer){       // If this cache level has stream buffer, check it for a hit
      uint32_t buffer_block_tag = addr >> num_block_offset;
      MRU_buffer_index = check_StreamBuffer(buffer_block_tag);
      if(MRU_buffer_index >= 0) StreamBuffer_read_hit = true;

      if(!cache_read_hit && !StreamBuffer_read_hit){     // Scenario # 1 (create a new prefetch stream): Requested block misses in cache and misses in the stream buffer
         StreamBuffer_read_request(buffer_block_tag);    // prefetch the next M consecutive memory blocks into the Stream Buffer.
         // handle the miss in cache as usual, same as follow but skip the following read miss operation
         num_read_miss++;
         make_space(index);
         if (next != nullptr) {
            next->read_request(addr);
         }
         install_block(index, tag);
         LRU_update(index, tag);
         num_read ++;
         return;
      }else if(!cache_read_hit && StreamBuffer_read_hit){   // Scenario # 2 (benefit from and continue a prefetch stream): Requested block X misses in CACHE and hits in the Stream Buffer
         make_space(index);                                 // First, make space in CACHE for the requested block X
         // --------------------------------------------------------------------
         // Second, copy the requested block X from the Stream Buffer into CACHE 
         // --------------------------------------------------------------------
         install_block(index, tag);
         LRU_update(index, tag);
         num_read ++;
         Prefetch_new_stream(MRU_buffer_index, buffer_block_tag); // Next, manage the Stream Buffer
         return;                        
      }else if(cache_read_hit && StreamBuffer_read_hit){    // Scenario #4 (continue prefetch stream to stay in sync with demand stream): Requested block X hits in CACHE and hits in the Stream Buffer
         // no transfer from Stream Buffer to CACHE.
         Prefetch_new_stream(MRU_buffer_index, buffer_block_tag);   
      }
      // Scenario # 3 (do nothing): Requested block X hits in CACHE and misses in the Stream Buffer
   }

   if(cache_read_hit){
      LRU_update(index, tag); // update the block LRU information of this specific set
      //--------------
      // return the required byte (not implement details in the cache simulator)
      //--------------
      num_read ++;
   }else{   // if read miss, issue read request to the next level
      num_read_miss ++;
      make_space(index);
      if (next != nullptr) {        // if next level is lower-level cache
         next->read_request(addr);
      }else{                        // next level is the main memory

      }
      install_block(index, tag); // install the block
      LRU_update(index, tag); // then update the block LRU information of this specific set
      //--------------
      // return the required byte (not implement details in the cache simulator)
      //--------------
      num_read ++;
   }
}

void CACHE::write_request(uint32_t addr){
   // Calculation based on the current cache level configuration
   uint32_t tag, index;
   unsigned int num_block_offset = int_log2(blocksize);
   unsigned int num_index_bits = int_log2(sets);
   uint32_t mask = ((1 << num_index_bits) - 1);
   index = (addr >> num_block_offset) & mask;
   tag = addr >> (num_block_offset + num_index_bits);

   bool cache_write_hit = false;
   for(uint32_t block_index=0; block_index<ways; block_index++){
      if(BLOCK[index][block_index].valid == true && 
         BLOCK[index][block_index].address == tag) {
         cache_write_hit = true;
         break;
      }
   }

   bool StreamBuffer_read_hit = false;
   int MRU_buffer_index = -1;
   if(hasStreamBuffer){       // If this cache level has stream buffer, check it for a hit
      uint32_t buffer_block_tag = addr >> num_block_offset;
      MRU_buffer_index = check_StreamBuffer(buffer_block_tag); // If find the block within the buffer, return the comparatively MRU buffer's index
      if(MRU_buffer_index >= 0) StreamBuffer_read_hit = true;

      if(!cache_write_hit && !StreamBuffer_read_hit){     // Scenario # 1 (create a new prefetch stream): Requested block misses in cache and misses in the stream buffer
         StreamBuffer_read_request(buffer_block_tag);    // prefetch the next M consecutive memory blocks into the Stream Buffer.
         // handle the miss in cache as usual, same as follow but skip the following read miss operation
         num_write_miss++;
         make_space(index);
         if (next != nullptr) {
            next->read_request(addr);
         }
         install_block(index, tag);
         LRU_update(index, tag);
         for(uint32_t block_index=0; block_index<ways; block_index++){
            if(BLOCK[index][block_index].valid &&
               BLOCK[index][block_index].address == tag){
               BLOCK[index][block_index].dirty = true;  // set dirty bit
               break;
            }
         }
         num_write ++;
         return;
      }else if(!cache_write_hit && StreamBuffer_read_hit){   // Scenario # 2 (benefit from and continue a prefetch stream): Requested block X misses in CACHE and hits in the Stream Buffer
         make_space(index);                                 // First, make space in CACHE for the requested block X
         // --------------------------------------------------------------------
         // Second, copy the requested block X from the Stream Buffer into CACHE 
         // --------------------------------------------------------------------
         install_block(index, tag);
         LRU_update(index, tag);
         for(uint32_t block_index=0; block_index<ways; block_index++){
            if(BLOCK[index][block_index].valid &&
               BLOCK[index][block_index].address == tag){
               BLOCK[index][block_index].dirty = true;  // set dirty bit
               break;
            }
         }
         num_write ++;
         Prefetch_new_stream(MRU_buffer_index, buffer_block_tag); // Next, manage the Stream Buffer
         return;               
      }else if(cache_write_hit && StreamBuffer_read_hit){    // Scenario #4 (continue prefetch stream to stay in sync with demand stream): Requested block X hits in CACHE and hits in the Stream Buffer
         // no transfer from Stream Buffer to CACHE.
         Prefetch_new_stream(MRU_buffer_index, buffer_block_tag);   
      }
      // Scenario # 3 (do nothing): Requested block X hits in CACHE and misses in the Stream Buffer
   }

   if(cache_write_hit){
      LRU_update(index, tag);
      //--------------
      // perform the CPU's write (not implement details in the cache simulator)
      //--------------
      for(uint32_t block_index=0; block_index<ways; block_index++){
         if(BLOCK[index][block_index].valid &&
            BLOCK[index][block_index].address == tag){
            BLOCK[index][block_index].dirty = true;  // set dirty bit
            break;
         }
      }
      num_write ++;
   }else{
      num_write_miss ++;
      make_space(index);
      if(next != nullptr){
         next->read_request(addr);   // issue read request to next level
      }else{
                                 // next level is main memory
      }
      install_block(index, tag);
      LRU_update(index, tag);
      //--------------
      // perform the CPU's write (not implement details in the cache simulator)
      //--------------
      for(uint32_t block_index=0; block_index<ways; block_index++){
         if(BLOCK[index][block_index].valid &&
            BLOCK[index][block_index].address == tag){
            BLOCK[index][block_index].dirty = true;  // set dirty bit
            break;
         }
      }
      num_write ++;

   }
}

void CACHE::LRU_update(uint32_t set_index, uint32_t tag_value){
   int LRU_caparison = 0;
   int hit_way = -1;
   for(uint32_t block_index=0; block_index<ways; block_index++){
      if(BLOCK[set_index][block_index].valid &&
         BLOCK[set_index][block_index].address == tag_value) {
         if(BLOCK[set_index][block_index].LRU == 0){
            return;  // the selected block is already MRU
         }
         LRU_caparison = BLOCK[set_index][block_index].LRU; // get the selected tag's LRU
         hit_way = block_index;
         break;
      }
   }
   for(uint32_t block_index=0; block_index<ways; block_index++){
      if((int)block_index == hit_way) continue;
      if(BLOCK[set_index][block_index].LRU < LRU_caparison){
         BLOCK[set_index][block_index].LRU ++;
      }
   }
   if(hit_way == -1) return;
   BLOCK[set_index][(uint32_t)hit_way].LRU = 0;  // set the selected tag's LRU to 0 (most recently used)
}

void CACHE::install_block(uint32_t set_index, uint32_t tag_value){
   // if there is at least one invalid block
   for(uint32_t block_index=0; block_index<ways; block_index++){
      if(!BLOCK[set_index][block_index].valid){
         BLOCK[set_index][block_index].address = tag_value;
         BLOCK[set_index][block_index].valid = true;  // update the block's valid bit
         BLOCK[set_index][block_index].dirty = false;
         return;
      }
   }

   // find the least recently uesd block
   int LRU_max = 0;
   uint32_t LRU_block_index = 0;
   for(uint32_t block_index=0; block_index<ways; block_index++){
      if(BLOCK[set_index][block_index].LRU > LRU_max){
         LRU_max = BLOCK[set_index][block_index].LRU;
         LRU_block_index = block_index;
      }
   }
   
   BLOCK[set_index][LRU_block_index].address = tag_value;   // install the block value
   BLOCK[set_index][LRU_block_index].valid = true;
   BLOCK[set_index][LRU_block_index].dirty = false;
}

void CACHE::make_space(uint32_t set_index){
   // if there is any invalid block, do nothing
   for(uint32_t block_index=0; block_index<ways; block_index++){
      if(!BLOCK[set_index][block_index].valid){
         return;
      }
   }

   // find the least recently uesd block
   int LRU_max = 0;
   uint32_t LRU_block_index = 0;
   for(uint32_t block_index=0; block_index<ways; block_index++){
      if(BLOCK[set_index][block_index].LRU > LRU_max){
         LRU_max = BLOCK[set_index][block_index].LRU;
         LRU_block_index = block_index;
      }
   }

   // if this victim block is dirty, write of the victim block to next level
   if(BLOCK[set_index][LRU_block_index].dirty == true){
      if(next != nullptr){
         uint32_t victim_tag = BLOCK[set_index][LRU_block_index].address;
         uint32_t block_addr = (victim_tag << int_log2(sets)) | set_index;
         uint32_t victim_addr = block_addr << int_log2(blocksize);
         next->write_request(victim_addr); // if next level is lower level cache, send write request
      }else{   // next level is main memory
         // write back to main memory, not show detail here
      }
      BLOCK[set_index][LRU_block_index].dirty = false;   // update the block's dirty bit
      num_write_back ++;
   }
}

void CACHE::print_cache_content(){
    for (uint32_t i = 0; i < sets; i++) {
        cout << "set";
        cout << setw(7) << i << ":" << "   ";

        vector<block_params> blocks = BLOCK[i];

        // sort block from MRU to LRU
        sort(blocks.begin(), blocks.end(),
             [](const block_params &a, const block_params &b) {
                 return a.LRU < b.LRU;
             });

        for (uint32_t j = 0; j < ways; j++) {
            if (blocks[j].dirty) {
                printf("%x D", blocks[j].address);
                cout << "  ";
            } else {
                printf("%x", blocks[j].address);
                cout << "    ";
            }
        }
        cout << "\n";
    }
    cout << "\n";
}

void CACHE::StreamBuffer_Setup(uint32_t PREF_N, uint32_t PREF_M){
   if(hasStreamBuffer){
      return;  // already has a valid size of stream buffer
   }else {
      hasStreamBuffer = true;
      StreamBuffer.resize(PREF_N);
      for(int i=0; i<(int)PREF_N; i++){
         StreamBuffer[i].setup(i, PREF_M);
      }
   }
}

int CACHE::check_StreamBuffer(uint32_t buffer_block_tag){
   int MRU_buffer_index = -1;
   int smallest_LRU = (int)StreamBuffer.size();
   for(int i=0; i<(int)StreamBuffer.size(); i++){
      for(int j=0; j<(int)StreamBuffer[i].BLOCK_ADDR.size(); j++){
         if(StreamBuffer[i].BLOCK_ADDR[j] == buffer_block_tag){   // find the block in this stream buffer
            if(StreamBuffer[i].LRU < smallest_LRU){
               smallest_LRU = StreamBuffer[i].LRU;
               MRU_buffer_index = i;
            }
         }
      }
   }
   if(MRU_buffer_index >= 0){
      // StreamBuffer_LRU_Update((uint32_t)MRU_buffer_index);  // find the block in the stream buffer, therefore update the LRU bit
      return MRU_buffer_index;
   }else{
      return -1;
   }
}

void CACHE::StreamBuffer_LRU_Update(uint32_t MRU_buffer_index){
   for(uint32_t i=0; i<StreamBuffer.size(); i++){
      if(i == MRU_buffer_index) continue;
      if(StreamBuffer[i].LRU < StreamBuffer[MRU_buffer_index].LRU){
         StreamBuffer[i].LRU ++;
      }
   }
   StreamBuffer[MRU_buffer_index].LRU = 0;
}

void CACHE::StreamBuffer_read_request(uint32_t buffer_block_tag){
   // prefetches are implemented by issuing read requests to the next level in the memory hierarchy.
   // find the LRU stream buffer
   int LRU_caparison = -1;
   int hit_buffer = -1;
   for(uint32_t i=0; i<StreamBuffer.size(); i++){
      if(StreamBuffer[i].LRU > LRU_caparison){
         LRU_caparison = StreamBuffer[i].LRU;
         hit_buffer = i;      // StreamBuffer[i] is the LRU one
      }
   }
   num_prefetch  = num_prefetch + StreamBuffer[0].BLOCK_ADDR.size();
   // prefetch the next M consecutive memory blocks into the LRU Stream Buffer.
   for(uint32_t j=0; j<StreamBuffer[hit_buffer].BLOCK_ADDR.size(); j++){
      StreamBuffer[hit_buffer].BLOCK_ADDR[j] = buffer_block_tag + j + 1;
   }
   StreamBuffer[hit_buffer].valid = true;    // set this stream buffer valid bit
   StreamBuffer_LRU_Update(hit_buffer);      // Update LRU bit of each stream buffer
}

void CACHE::Prefetch_new_stream(uint32_t buffer_index, uint32_t buffer_block_tag){
   count_num_prefetch(buffer_index, buffer_block_tag);
   for(uint32_t j=0; j<StreamBuffer[buffer_index].BLOCK_ADDR.size(); j++){
      StreamBuffer[buffer_index].BLOCK_ADDR[j] = buffer_block_tag + j + 1;
   }
   StreamBuffer[buffer_index].valid = true;
   StreamBuffer_LRU_Update(buffer_index);
}

void CACHE::count_num_prefetch(uint32_t buffer_index, uint32_t buffer_block_tag){
   if(!StreamBuffer[buffer_index].valid){
      num_prefetch = num_prefetch + StreamBuffer[buffer_index].BLOCK_ADDR.size();
   }else{
      for(uint32_t i=0; i<StreamBuffer[buffer_index].BLOCK_ADDR.size(); i++){
         if(StreamBuffer[buffer_index].BLOCK_ADDR[i] == buffer_block_tag){
            num_prefetch = num_prefetch + i + 1;
            break;
         }
      }
   }
}

void CACHE::print_StreamBuffer_content(){
   if(hasStreamBuffer){
      vector<STREAM_BUFFER> SB = StreamBuffer;
      // sort block from MRU to LRU
      sort(SB.begin(), SB.end(),
            [](const STREAM_BUFFER &a, const STREAM_BUFFER &b) {
               return a.LRU < b.LRU;
            });
      for(uint32_t i=0; i<SB.size(); i++){
         for(uint32_t j=0; j<SB[i].BLOCK_ADDR.size(); j++){
            printf(" %x ", SB[i].BLOCK_ADDR[j]);
         }
         cout << "\n";
      }
      cout << "\n";
   }
}


/*  "argc" holds the number of command-line arguments.
    "argv[]" holds the arguments themselves.

    Example:
    ./sim 32 8192 4 262144 8 3 10 gcc_trace.txt
    argc = 9
    argv[0] = "./sim"
    argv[1] = "32"
    argv[2] = "8192"
    ... and so on
*/
int main (int argc, char *argv[]) {
   FILE *fp;			// File pointer.
   char *trace_file;		// This variable holds the trace file name.
   cache_params_t params;	// Look at the sim.h header file for the definition of struct cache_params_t.
   char rw;			// This variable holds the request's type (read or write) obtained from the trace.
   uint32_t addr;		// This variable holds the request's address obtained from the trace.
				// The header file <inttypes.h> above defines signed and unsigned integers of various sizes in a machine-agnostic way.  "uint32_t" is an unsigned integer of 32 bits.

   // Exit with an error if the number of command-line arguments is incorrect.
   if (argc != 9) {
      printf("Error: Expected 8 command-line arguments but was provided %d.\n", (argc - 1));
      exit(EXIT_FAILURE);
   }
   
   // "atoi()" (included by <stdlib.h>) converts a string (char *) to an integer (int).
   params.BLOCKSIZE = (uint32_t) atoi(argv[1]);
   params.L1_SIZE   = (uint32_t) atoi(argv[2]);
   params.L1_ASSOC  = (uint32_t) atoi(argv[3]);
   params.L2_SIZE   = (uint32_t) atoi(argv[4]);
   params.L2_ASSOC  = (uint32_t) atoi(argv[5]);
   params.PREF_N    = (uint32_t) atoi(argv[6]);
   params.PREF_M    = (uint32_t) atoi(argv[7]);
   trace_file       = argv[8];

   // Open the trace file for reading.
   fp = fopen(trace_file, "r");
   if (fp == (FILE *) NULL) {
      // Exit with an error if file open failed.
      printf("Error: Unable to open file %s\n", trace_file);
      exit(EXIT_FAILURE);
   }
    
   // Print simulator configuration.
   printf("===== Simulator configuration =====\n");
   printf("BLOCKSIZE:  %u\n", params.BLOCKSIZE);
   printf("L1_SIZE:    %u\n", params.L1_SIZE);
   printf("L1_ASSOC:   %u\n", params.L1_ASSOC);
   printf("L2_SIZE:    %u\n", params.L2_SIZE);
   printf("L2_ASSOC:   %u\n", params.L2_ASSOC);
   printf("PREF_N:     %u\n", params.PREF_N);
   printf("PREF_M:     %u\n", params.PREF_M);
   printf("trace_file: %s\n", trace_file);
   printf("\n");

   // L1 cache parameters calculation and configuration setup
   uint32_t L1_sets;
   L1_sets = params.L1_SIZE / (params.L1_ASSOC * params.BLOCKSIZE);
   CACHE L1_cache(L1_sets, params.L1_ASSOC, params.BLOCKSIZE, 0, 0); // Set prefetch unit size later if L1 has it

   // L2 cache parameters calculation and configuration setup
   uint32_t L2_sets = 0;
   if (params.L2_SIZE != 0 && params.L2_ASSOC !=0){
      L2_sets = params.L2_SIZE / (params.L2_ASSOC * params.BLOCKSIZE);
   }
   CACHE L2_cache(L2_sets, params.L2_ASSOC, params.BLOCKSIZE, 0, 0); // Set prefetch unit size later if L2 has it

   if (params.L2_SIZE != 0 && params.L2_ASSOC !=0){                  // L2 cache level exists
      L1_cache.next = &L2_cache;                                     // Set L2 cache as the next level of L1 cache
      if(params.PREF_N != 0 && params.PREF_M != 0){
         L2_cache.StreamBuffer_Setup(params.PREF_N, params.PREF_M);  // Valid prefetch unit size, so set it for L2 cache
      }
   }else{                                                            // Only L1 cache level exists
      if(params.PREF_N != 0 && params.PREF_M != 0){
         L1_cache.StreamBuffer_Setup(params.PREF_N, params.PREF_M);  // Valid prefetch unit size, so set it for L1 cache
      }
   }

   // Read requests from the trace file and echo them back.
   while (fscanf(fp, "%c %x\n", &rw, &addr) == 2) {	// Stay in the loop if fscanf() successfully parsed two tokens as specified.
      if (rw == 'r') {
         L1_cache.read_request(addr);
      } else if (rw == 'w') {
         L1_cache.write_request(addr);
      } else {
         printf("Error: Unknown request type %c.\n", rw);
	      exit(EXIT_FAILURE);
      }
   }

   double L1_miss_rate = (double)(L1_cache.num_read_miss + L1_cache.num_write_miss) / (double)(L1_cache.num_read + L1_cache.num_write);
   double L2_miss_rate = 0.0000;
   int total_memory_traffic = 0;

   cout << "===== L1 contents =====" << endl;
   L1_cache.print_cache_content();
   

   if(params.L2_SIZE != 0 && params.L2_ASSOC !=0){
      cout << "===== L2 contents =====" << endl;
      L2_cache.print_cache_content();
      L2_miss_rate = (double)L2_cache.num_read_miss / (double)L2_cache.num_read;
      total_memory_traffic = L2_cache.num_read_miss + L2_cache.num_write_miss + L2_cache.num_write_back + L2_cache.num_prefetch; // L2 prefetch to be added
   }else{
      total_memory_traffic = L1_cache.num_read_miss + L1_cache.num_write_miss + L1_cache.num_write_back + L1_cache.num_prefetch; // L1 prefetch to be added
   }

   if(params.PREF_N != 0 && params.PREF_M != 0){
      cout << "===== Stream Buffer(s) contents =====" << endl;
      L1_cache.print_StreamBuffer_content(); // if L1 cache has no any Stream Buffer, here will print nothing
      L2_cache.print_StreamBuffer_content();
   }


   /* 
   For this project, prefetching is only tested and explored in the last-level cache of the memory hierarchy.
   This means that the measurements j and k should always be 0 because the L1 will not issue prefetch requests 
   to the L2. Nonetheless, a well-done implementation of a generic CACHE will distinguish incoming demand read 
   requests from incoming prefetch read requests, even though in this project the distinction will not be exercised. 
   */
   cout << "===== Measurements =====" << endl;
   cout << "a. L1 reads:                   " << L1_cache.num_read << endl;
   cout << "b. L1 read misses:             " << L1_cache.num_read_miss << endl;
   cout << "c. L1 writes:                  " << L1_cache.num_write<< endl;
   cout << "d. L1 write misses:            " << L1_cache.num_write_miss << endl;
   cout << "e. L1 miss rate:               " << fixed << setprecision(4) << L1_miss_rate << endl;
   cout << "f. L1 writebacks:              " << L1_cache.num_write_back << endl;
   cout << "g. L1 prefetches:              " << L1_cache.num_prefetch << endl;
   cout << "h. L2 reads (demand):          " << L2_cache.num_read << endl;
   cout << "i. L2 read misses (demand):    " << L2_cache.num_read_miss << endl;
   cout << "j. L2 reads (prefetch):        " << 0 << endl;  // number of L2 reads that originated from L1 prefetches (should match g: L1 prefetches)
   cout << "k. L2 read misses (prefetch):  " << 0 << endl;  /* number of L2 read misses that originated from L1 prefetches, excluding such L2 read misses 
                                                               that hit in the stream buffers if L2 prefetch unit is enabled*/
   cout << "l. L2 writes:                  " << L2_cache.num_write << endl;
   cout << "m. L2 write misses:            " << L2_cache.num_write_miss << endl;
   cout << "n. L2 miss rate:               " << fixed << setprecision(4) << L2_miss_rate << endl;
   cout << "o. L2 writebacks:              " << L2_cache.num_write_back << endl;
   cout << "p. L2 prefetches:              " << L2_cache.num_prefetch << endl;
   cout << "q. memory traffic:             " << total_memory_traffic << endl;
   return(0);
}


unsigned int int_log2(uint32_t x) {
   unsigned int r = 0;
   while (x >>= 1) {
        ++r;
   }
   return r;
}
