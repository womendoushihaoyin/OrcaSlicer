// Headless measurements of production components; this is not a GUI startup benchmark.
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/TextureDisplacementConsumer.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/host/PluginResourceTextures.hpp"
#include "slic3r/plugin/pluginTypes/pages/PagesPluginCapability.hpp"
#include <boost/log/core.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <pybind11/embed.h>
#include <tbb/task_arena.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace Slic3r;
namespace bench_fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void save(const bench_fs::path& path, const json& report)
{
    std::ofstream file(path); require(bool(file), "Cannot write benchmark report"); file << report.dump(2) << '\n';
}
template<class Fn> void timed(json& report, const std::string& stage, Fn fn)
{
    const auto begin = Clock::now();
    try { fn(); }
    catch (...) {
        report["stages"][stage] = {{"milliseconds", std::chrono::duration<double,std::milli>(Clock::now()-begin).count()}, {"passed",false}};
        throw;
    }
    report["stages"][stage] = {{"milliseconds",std::chrono::duration<double,std::milli>(Clock::now()-begin).count()}, {"passed",true}};
}
std::map<std::string,std::string> keys(PluginManager& manager)
{
    std::map<std::string,std::string> result;
    for (const auto& descriptor : manager.get_plugin_descriptors()) {
        const auto name = bench_fs::u8path(descriptor.entry_path).filename().u8string();
        for (const auto& kind : {"script","pages","slicing","texture"})
            if (name.find(std::string("orca_benchmark_")+kind+"-") == 0) result[kind]=descriptor.plugin_key;
    }
    return result;
}
void load_enabled(PluginManager& manager, json& report)
{
    auto list = manager.get_enabled_plugin_keys();
    std::sort(list.begin(),list.end());
    report["autoload_packages"] = list.size();
    timed(report,"plugin_load_sequential",[&] {
        for (const auto& key : list) {
            manager.load_plugin(key);
            std::string error;
            require(manager.wait_for_plugin_load(key,std::chrono::seconds(45),error),error);
        }
    });
}
void slicing(PluginManager& manager, const std::map<std::string,std::string>& packages, bool enabled,
             const bench_fs::path& output, json& report)
{
    Model model;
    auto* object=model.add_object(); object->name="benchmark-cube";
    object->add_volume(TriangleMesh(make_cube(20.,20.,2.))); object->add_instance(); object->ensure_on_bed();
    DynamicPrintConfig config=DynamicPrintConfig::full_print_config();
    config.set_key_value("layer_height",new ConfigOptionFloat(.2));
    config.set_key_value("initial_layer_print_height",new ConfigOptionFloat(.2));
    config.set_key_value("elefant_foot_compensation",new ConfigOptionFloat(0.));
    if (enabled) {
        config.set_key_value("slicing_pipeline_plugin",new ConfigOptionStrings({"Benchmark Slice"}));
        config.set_key_value("plugins",new ConfigOptionStrings({packages.at("slicing")+";;Benchmark Slice"}));
    }
    Print print;
    print.is_BBL_printer()=false;
    print.set_status_silent(); print.set_cancel_callback([]{});
    timed(report,"print_apply",[&]{
        print.apply(model,config); print.restart();
        // A standalone fixture must provide stable export IDs even when object exclusion is off.
        // The current production constructor leaves this field unset until set_object_info().
        size_t object_id=0;
        for (auto* item : print.objects()) item->set_id(object_id++);
    });
    timed(report,"print_process",[&]{tbb::task_arena(1).execute([&]{print.process();});});
    size_t layers=0;
    for (const auto* item:print.objects()) layers+=item->layers().size();
    require(layers>0,"No actual layers produced");
    GCodeProcessorResult processor;
    const auto gcode=output.parent_path()/"native.gcode";
    timed(report,"gcode_export",[&]{print.export_gcode(gcode.u8string(),&processor,nullptr);});
    require(!processor.moves.empty()&&bench_fs::file_size(gcode)>0,"No actual G-code moves produced");
    report["slice"]={{"layers",layers},{"gcode_moves",processor.moves.size()},{"gcode",gcode.u8string()},
                     {"model","20x20x2mm cube"},{"tbb_arena_concurrency",1}};
}
void mesh_access(int side, json& report)
{
    auto mesh=std::make_shared<TriangleMesh>();
    timed(report,"mesh_construct",[&]{
        indexed_triangle_set its;
        its.vertices.reserve(size_t(side+1)*size_t(side+1)); its.indices.reserve(size_t(side)*size_t(side)*2);
        for(int y=0;y<=side;++y)for(int x=0;x<=side;++x)its.vertices.emplace_back(float(x),float(y),0.f);
        for(int y=0;y<side;++y)for(int x=0;x<side;++x){
            int a=y*(side+1)+x,b=a+1,c=a+side+1,d=c+1;
            its.indices.emplace_back(a,b,d);its.indices.emplace_back(a,d,c);
        }
        *mesh=TriangleMesh(std::move(its));
    });
    PythonGILState gil; require(bool(gil),"No GIL for real mesh bindings");
    namespace py=pybind11;
    py::module_::import("orca");
    py::dict globals; globals["mesh"]=py::cast(mesh); globals["side"]=side;
    timed(report,"mesh_python_scalar_access",[&]{
        py::exec(R"PY(
checksum = 0.0
for i in range(mesh.vertex_count()):
    x,y,z = mesh.vertex(i)
    checksum += x+y+z
index_checksum = 0
for i in range(mesh.triangle_count()):
    a,b,c = mesh.triangle(i)
    index_checksum += a+b+c
assert checksum == side*(side+1)*(side+1)
assert mesh.triangle_count() == side*side*2
assert index_checksum == 3*side**3*(side+2)
)PY",globals);
    });
    report["mesh"]={{"vertices",mesh->its.vertices.size()},{"triangles",mesh->its.indices.size()},
                    {"scalar_calls",mesh->its.vertices.size()+mesh->its.indices.size()},
                    {"checksum",globals["checksum"].cast<double>()},
                    {"numpy_bulk","pending; NumPy is not installed in this dependency-free fixture"}};
}
void callbacks(PluginManager& manager,const std::map<std::string,std::string>& packages,int calls,json& report)
{
    std::string error;
    timed(report,"script_callbacks",[&]{
        for(int i=1;i<=calls;++i){
            const auto reply=manager.run_script_capability(packages.at("script"),"Benchmark Script",error);
            require(error.empty()&&reply.status==PluginResult::Success&&reply.data==std::to_string(i),"Script callback failed: "+error);
        }
    });
    {
        auto page=std::dynamic_pointer_cast<PagesPluginCapability>(manager.acquire_capability(
            {PluginCapabilityType::Pages,"Benchmark Page",packages.at("pages")},error));
        require(bool(page),error);
        std::string reply;
        page->set_message_sender([&](const std::string& value){reply=value;});
        timed(report,"pages_html",[&]{require(page->get_ui().find("Benchmark Page")!=std::string::npos,"Missing page HTML");});
        timed(report,"pages_headless_message_roundtrips",[&]{
            for(int i=1;i<=calls;++i){
                page->on_message("{\"value\":7}");
                const auto data=json::parse(reply);
                require(data.at("count")==i&&data.at("reply").at("value")==7,"Page bridge reply mismatch");
            }
        });
        page->clear_message_sender();
    }
    const auto entries=PluginResourceTextures::instance().entries();
    require(entries.size()==1,"Resource registry did not finish real load callback");
    const int resource_calls=std::min(calls,100);
    timed(report,"texture_owned_read_and_decode",[&]{
        for(int i=0;i<resource_calls;++i){
            const auto bytes=PluginResourceTextures::instance().read_texture(entries.front().id,
                (bench_fs::u8path(entries.front().texture_dir)/bench_fs::u8path("height-纹理.png")).u8string(),error);
            require(!bytes.empty(),error);
            const auto decoded=decode_texture_image(bytes,error);
            require(!decoded.empty()&&decoded.width==512&&decoded.height==512,error);
        }
    });
    report["callbacks"]={{"script",calls},{"pages",calls},{"texture_read_decode",resource_calls},
                         {"webview_render","pending; headless bridge only"}};
}
}
int main(int argc,char**argv)
{
    boost::nowide::args utf8_args(argc,argv); boost::nowide::nowide_filesystem();
    if(argc!=9)return 2;
    const std::string mode=argv[1],state=argv[2],workload=argv[3];
    const bench_fs::path fixtures=bench_fs::u8path(argv[4]),profile=bench_fs::u8path(argv[5]),output=bench_fs::u8path(argv[6]);
    if((mode!="prepare"&&mode!="run")||(state!="none"&&state!="disabled"&&state!="enabled")||
       (workload!="startup"&&workload!="slice"&&workload!="mesh"&&workload!="callbacks"))return 2;
    int calls=0,side=0;
    try{calls=std::stoi(argv[7]);side=std::stoi(argv[8]);}catch(...){return 2;}
    if(calls<1||calls>1000000||side<1||side>2048||bench_fs::exists(output))return 2;
    const auto marker=profile/"benchmark-profile.json";
    if(mode=="prepare") {if(bench_fs::exists(profile))return 3;bench_fs::create_directories(profile);save(marker,{{"schema",1},{"state",state}});}
    else {
        std::ifstream file(marker); if(!file)return 3; const auto saved=json::parse(file);
        if(saved.value("schema",0)!=1||saved.value("state","")!=state)return 3;
    }
    bench_fs::create_directories(output.parent_path()); set_data_dir(profile.u8string());
    boost::log::core::get()->set_logging_enabled(false);
    auto& manager=PluginManager::instance();
    json report={{"schema",1},{"mode",mode},{"state",state},{"workload",workload},{"passed",false},
                 {"measurement_scope","headless production components; not GUI startup"},{"stages",json::object()}};
    int result=0;
    std::map<std::string,std::string> storage;
    try {
        timed(report,"runtime_initialize",[&]{require(manager.initialize(),PythonInterpreter::instance().last_error());});
        if(mode=="prepare"&&state!="none") {
            timed(report,"install_excluded_from_samples",[&]{
                for(const auto& kind:{"script","pages","slicing","texture"}){
                    std::string error;PluginDescriptor descriptor;
                    require(manager.install_plugin((fixtures/(std::string("orca_benchmark_")+kind+"-1.0.0-py3-none-any.whl")).u8string(),descriptor,error),error);
                }
            });
        }
        timed(report,"manifest_discovery",[&]{manager.discover_plugins(false,true);});
        require(manager.get_discovery_error().empty(),manager.get_discovery_error());
        auto packages=keys(manager);
        require(packages.size()==(state=="none"?0:4),"Prepared package inventory mismatch");
        if(mode=="prepare"&&state!="none") {
            timed(report,"activation_setup_excluded_from_samples",[&]{
                for(const auto& item:packages){manager.load_plugin(item.second);std::string error;
                    require(manager.wait_for_plugin_load(item.second,std::chrono::seconds(45),error),error);
                    if(state=="disabled") {
                        for(const auto& cap:manager.get_plugin_capabilities(item.second))
                            require(manager.set_capability_enabled(cap->identity(),false),"Cannot disable prepared capability");
                        require(manager.unload_plugin(item.second),"Cannot disable prepared package");
                    }
                }
            });
        } else load_enabled(manager,report);
        report["installed_packages"]=packages.size();
        report["enabled_capabilities"]=manager.get_plugin_capabilities().size();
        require(manager.get_plugin_capabilities().size()==(state=="enabled"?4:0),"Enabled capability inventory mismatch");
        for(const auto& item:packages)storage[item.first]=manager.get_storage_dir(item.second);
        if(mode=="run") {
            if(workload=="slice")slicing(manager,packages,state=="enabled",output,report);
            else if(workload=="mesh")mesh_access(side,report);
            else if(workload=="callbacks"){
                require(state=="enabled","Callback workload requires enabled packages");callbacks(manager,packages,calls,report);
            }
        }
        report["passed"]=true;
    }catch(const std::exception& e){report["error"]=e.what();result=1;}
    timed(report,"manager_shutdown",[&]{manager.shutdown();});
    timed(report,"interpreter_shutdown",[&]{PythonInterpreter::instance().shutdown();});
    if(state=="enabled") {
        for(const auto& item:storage){std::ifstream file(bench_fs::u8path(item.second)/"benchmark-summary.json");
            if(file)report["package_counters"][item.first]=json::parse(file);}
        if(mode=="run"&&workload=="slice"){
            const int executed=report.value("package_counters",json::object()).value("slicing",json::object()).value("calls",0);
            if(executed==0){report["passed"]=false;report["error"]="Real slicing dispatcher made no plugin calls";result=1;}
        }
    }
    save(output,report);std::cout<<(result==0?"PASS ":"FAIL ")<<"benchmark "<<state<<" "<<workload<<'\n';
    return result;
}
