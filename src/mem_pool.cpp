/*************************************************
* Pooling Allocator Source File                  *
* (C) 1999-2006 The Botan Project                *
*************************************************/

#include <botan/mem_pool.h>
#include <botan/libstate.h>
#include <botan/config.h>
#include <botan/bit_ops.h>
#include <botan/util.h>
#include <algorithm>

namespace Botan {

namespace {

/*************************************************
* Decide how much memory to allocate at once     *
*************************************************/
u32bit choose_pref_size(u32bit provided)
   {
   if(provided)
      return provided;

   u32bit result = Config::get_u32bit("base/memory_chunk");
   if(result)
      return result;

   return 16*1024;
   }

}

/*************************************************
* Memory_Block Constructor                       *
*************************************************/
Pooling_Allocator::Memory_Block::Memory_Block(void* buf, u32bit map_size,
                                              u32bit block_size)
   {
   buffer = static_cast<byte*>(buf);
   bitmap = 0;
   this->block_size = block_size;

   buffer_end = buffer + (block_size * BITMAP_SIZE);

   clear_mem(buffer, block_size * BITMAP_SIZE);

   if(map_size != BITMAP_SIZE)
      throw Invalid_Argument("Memory_Block: Bad bitmap size");
   }

/*************************************************
* Compare a Memory_Block with a void pointer     *
*************************************************/
bool Pooling_Allocator::Memory_Block::operator<(const void* other) const
   {
   if(buffer <= other && other < buffer_end)
      return false;
   return (buffer < other);
   }

/*************************************************
* Compare two Memory_Block objects               *
*************************************************/
bool Pooling_Allocator::Memory_Block::operator<(const Memory_Block& blk) const
   {
   return (buffer < blk.buffer);
   }

/*************************************************
* See if ptr is contained by this block          *
*************************************************/
bool Pooling_Allocator::Memory_Block::contains(void* ptr,
                                               u32bit length) const throw()
   {
   return ((buffer <= ptr) &&
           (buffer_end >= (byte*)ptr + length * block_size));
   }

/*************************************************
* Allocate some memory, if possible              *
*************************************************/
byte* Pooling_Allocator::Memory_Block::alloc(u32bit n) throw()
   {
   if(n == 0 || n > BITMAP_SIZE)
      return 0;

   if(n == BITMAP_SIZE)
      {
      if(bitmap)
         return 0;
      else
         {
         bitmap = ~bitmap;
         return buffer;
         }
      }

   bitmap_type mask = ((bitmap_type)1 << n) - 1;
   u32bit offset = 0;

   while(bitmap & mask)
      {
      mask <<= 1;
      ++offset;

      if((bitmap & mask) == 0)
         break;
      if(mask >> 63)
         break;
      }

   if(bitmap & mask)
      return 0;

   bitmap |= mask;
   return buffer + offset * block_size;
   }

/*************************************************
* Mark this memory as free, if we own it         *
*************************************************/
void Pooling_Allocator::Memory_Block::free(void* ptr, u32bit blocks) throw()
   {
   clear_mem((byte*)ptr, blocks * block_size);

   const u32bit offset = ((byte*)ptr - buffer) / block_size;

   if(offset == 0 && blocks == BITMAP_SIZE)
      bitmap = ~bitmap;
   else
      {
      for(u32bit j = 0; j != blocks; ++j)
         bitmap &= ~((bitmap_type)1 << (j+offset));
      }
   }

/*************************************************
* Pooling_Allocator Constructor                  *
*************************************************/
Pooling_Allocator::Pooling_Allocator(u32bit p_size, bool) :
   PREF_SIZE(choose_pref_size(p_size)), BLOCK_SIZE(64)
   {
   mutex = global_state().get_mutex();
   last_used = blocks.begin();
   }

/*************************************************
* Pooling_Allocator Destructor                   *
*************************************************/
Pooling_Allocator::~Pooling_Allocator()
   {
   delete mutex;
   if(blocks.size())
      throw Invalid_State("Pooling_Allocator: Never released memory");
   }

/*************************************************
* Allocate some initial buffers                  *
*************************************************/
void Pooling_Allocator::init()
   {
   Mutex_Holder lock(mutex);

   get_more_core(PREF_SIZE);
   }

/*************************************************
* Free all remaining memory                      *
*************************************************/
void Pooling_Allocator::destroy()
   {
   Mutex_Holder lock(mutex);

   blocks.clear();

   for(u32bit j = 0; j != allocated.size(); ++j)
      dealloc_block(allocated[j].first, allocated[j].second);
   allocated.clear();
   }

/*************************************************
* Allocation                                     *
*************************************************/
void* Pooling_Allocator::allocate(u32bit n)
   {
   const u32bit BITMAP_SIZE = Memory_Block::bitmap_size();

   Mutex_Holder lock(mutex);

   if(n <= BITMAP_SIZE * BLOCK_SIZE)
      {
      const u32bit block_no = round_up(n, BLOCK_SIZE) / BLOCK_SIZE;

      byte* mem = allocate_blocks(block_no);
      if(mem)
         return mem;

      get_more_core(PREF_SIZE);

      mem = allocate_blocks(block_no);
      if(mem)
         return mem;

      throw Memory_Exhaustion();
      }

   void* new_buf = alloc_block(n);
   if(new_buf)
      return new_buf;

   throw Memory_Exhaustion();
   }

/*************************************************
* Deallocation                                   *
*************************************************/
void Pooling_Allocator::deallocate(void* ptr, u32bit n)
   {
   const u32bit BITMAP_SIZE = Memory_Block::bitmap_size();

   if(ptr == 0 && n == 0)
      return;

   Mutex_Holder lock(mutex);

   if(n > BITMAP_SIZE * BLOCK_SIZE)
      dealloc_block(ptr, n);
   else
      {
      const u32bit block_no = round_up(n, BLOCK_SIZE) / BLOCK_SIZE;

      std::vector<Memory_Block>::iterator i =
         std::lower_bound(blocks.begin(), blocks.end(), ptr);

      if(i != blocks.end() && i->contains((byte*)ptr, block_no))
         i->free(ptr, block_no);
      else
         throw Invalid_State("Pointer released to the wrong allocator");
      }
   }

/*************************************************
* Try to get some memory from an existing block  *
*************************************************/
byte* Pooling_Allocator::allocate_blocks(u32bit n)
   {
   if(blocks.empty())
      return 0;

   std::vector<Memory_Block>::iterator i = last_used;

   do
      {
      ++i;
      if(i == blocks.end())
         i = blocks.begin();

      byte* mem = i->alloc(n);
      if(mem)
         {
         last_used = i;
         return mem;
         }
      }
   while(i != last_used);

   return 0;
   }

/*************************************************
* Allocate more memory for the pool              *
*************************************************/
void Pooling_Allocator::get_more_core(u32bit in_bytes)
   {
   const u32bit BITMAP_SIZE = Memory_Block::bitmap_size();

   const u32bit TOTAL_BLOCK_SIZE = BLOCK_SIZE * BITMAP_SIZE;

   const u32bit in_blocks = round_up(in_bytes, BLOCK_SIZE) / TOTAL_BLOCK_SIZE;
   const u32bit to_allocate = in_blocks * TOTAL_BLOCK_SIZE;

   void* ptr = alloc_block(to_allocate);
   if(ptr == 0)
      throw Memory_Exhaustion();

   allocated.push_back(std::make_pair(ptr, to_allocate));

   for(u32bit j = 0; j != in_blocks; ++j)
      {
      byte* byte_ptr = static_cast<byte*>(ptr);

      blocks.push_back(
         Memory_Block(byte_ptr + j * TOTAL_BLOCK_SIZE, BITMAP_SIZE, BLOCK_SIZE)
         );
      }

   std::sort(blocks.begin(), blocks.end());
   last_used = std::lower_bound(blocks.begin(), blocks.end(), ptr);
   }

}
