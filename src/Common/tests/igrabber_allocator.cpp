#include <Common/Allocators/IGrabberAllocator.h>
#include <Common/Allocators/allocatorCommon.h>
#include <thread>

using namespace DB;

using Alloc = IGrabberAllocator<int, int>;

struct Holder
{
    Holder(Alloc& a, int key)
    {
        ptr = a.getOrSet(key,
            []{ return sizeof(int); },
            [key](void *){ return key; }).first;
    }

    std::shared_ptr<int> ptr;
};

int main() noexcept
{
   Alloc cache(MMAP_THRESHOLD);

   std::vector<std::thread> thread_pool;

   for (size_t k = 0; k < 2; k++)
       thread_pool.emplace_back([&cache] {
           for (int i = 1; i < 10000; ++i) {
               Holder inc(cache, 1);
           }});

   for (auto& t : thread_pool) t.join();
}

