// Standalone executable: allocator instrumentation must never enter the shared test runner.
#include "libslic3r/Print.hpp"
#include <boost/log/core.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
std::atomic<bool> poison_enabled{false};
std::atomic<size_t> poison_count{0};
std::array<std::atomic<void*>,64> poisoned_allocations{};
void poison(void* ptr,size_t size)
{
    if (size==sizeof(Slic3r::PrintObject) && poison_enabled.load(std::memory_order_relaxed)) {
        std::memset(ptr,0xa5,size);
        const size_t index=poison_count.fetch_add(1,std::memory_order_relaxed);
        if(index<poisoned_allocations.size()) poisoned_allocations[index].store(ptr,std::memory_order_relaxed);
    }
}
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct PoisonScope {
    PoisonScope(){poison_enabled.store(true,std::memory_order_relaxed);}
    ~PoisonScope(){poison_enabled.store(false,std::memory_order_relaxed);}
};
}
void* operator new(size_t size)
{
    void* ptr=std::malloc(size?size:1);if(!ptr)throw std::bad_alloc();poison(ptr,size);return ptr;
}
void* operator new[](size_t size){return ::operator new(size);}
void operator delete(void* ptr) noexcept{std::free(ptr);}
void operator delete[](void* ptr) noexcept{std::free(ptr);}
void operator delete(void* ptr,size_t) noexcept{std::free(ptr);}
void operator delete[](void* ptr,size_t) noexcept{std::free(ptr);}
void* operator new(size_t size,std::align_val_t alignment)
{
    void* ptr=nullptr;
#ifdef _WIN32
    ptr=_aligned_malloc(size?size:1,static_cast<size_t>(alignment));
#else
    if(posix_memalign(&ptr,static_cast<size_t>(alignment),size?size:1)!=0)ptr=nullptr;
#endif
    if(!ptr)throw std::bad_alloc();poison(ptr,size);return ptr;
}
void* operator new[](size_t size,std::align_val_t alignment){return ::operator new(size,alignment);}
void operator delete(void* ptr,std::align_val_t) noexcept
{
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
void operator delete[](void* ptr,std::align_val_t alignment) noexcept{::operator delete(ptr,alignment);}
void operator delete(void* ptr,size_t,std::align_val_t alignment) noexcept{::operator delete(ptr,alignment);}
void operator delete[](void* ptr,size_t,std::align_val_t alignment) noexcept{::operator delete(ptr,alignment);}

int main(int argc,char** argv)
{
    boost::nowide::args utf8_args(argc,argv);boost::nowide::nowide_filesystem();
    if(argc!=1)return 2;
    boost::log::core::get()->set_logging_enabled(false);
    try {
        using namespace Slic3r;
        Model model;auto* object=model.add_object();object->name="poisoned-constructor";
        object->add_volume(TriangleMesh(make_cube(20.,20.,2.)));object->add_instance();
        DynamicPrintConfig config=DynamicPrintConfig::full_print_config();
        Print print;print.is_BBL_printer()=false;print.set_status_silent();print.set_cancel_callback([]{});
        {PoisonScope active;print.apply(model,config);}
        require(print.objects().size()==1,"Expected one actual PrintObject");
        auto* actual=print.objects().front();bool confirmed=false;
        for(const auto& entry:poisoned_allocations)confirmed=confirmed||entry.load(std::memory_order_relaxed)==actual;
        require(confirmed,"The actual PrintObject allocation was not poisoned; test cannot establish initialization");
        if(actual->get_id()!=0){
            std::cerr<<"FAIL constructor left poisoned object id "<<actual->get_id()<<'\n';return 1;
        }
        for(auto* item:print.objects()) item->set_id(42);
        require(actual->get_id()==42,"Explicit export object numbering no longer works");
        std::cout<<"PASS constructor initialized actual poisoned PrintObject storage; explicit set_id preserved\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
