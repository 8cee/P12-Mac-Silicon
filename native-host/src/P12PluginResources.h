#pragma once
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
namespace p12 {
namespace fs=std::filesystem;
struct ResourceLocations {
    std::vector<fs::path> directories, platformLibraries;
    fs::path icu;
};
inline std::string lowerResourceName(std::string s){
    for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline bool sameResourceProduct(const std::string& name,const std::string& product){
    const auto n=lowerResourceName(name),p=lowerResourceName(product);
    return n==p||(n.rfind(p,0)==0 && n.size()>p.size() &&
      (n[p.size()]==' '||n[p.size()]=='_'||n[p.size()]=='-'||n[p.size()]=='.'));
}
// Only this product's roots are searched recursively. A sibling Ozone 11,
// another iZotope product or Electron's ICU must never be selected for Ozone 12.
inline ResourceLocations discoverPluginResources(const fs::path& plugin,
        const std::vector<fs::path>& vendorParents,const fs::path& explicitRoot={}){
    ResourceLocations result;
    std::error_code ec;
    std::vector<fs::path> roots;
    auto addDir=[&](const fs::path& p){
        if(fs::is_directory(p,ec)&&std::find(result.directories.begin(),result.directories.end(),p)==result.directories.end())
            result.directories.push_back(p);
    };
    const bool bundle=fs::is_directory(plugin,ec);
    const auto product=plugin.stem().string();
    addDir(bundle?plugin:plugin.parent_path());
    if(bundle)roots.push_back(plugin);
    // A direct ICU beside a single-file plug-in is a supported install layout.
    const auto direct=(bundle?plugin:plugin.parent_path())/"icudtl.dat";
    if(fs::is_regular_file(direct,ec))result.icu=direct;
    auto parents=vendorParents;parents.push_back(plugin.parent_path());
    for(const auto& parent:parents){
        if(!fs::is_directory(parent,ec))continue;
        for(fs::directory_iterator i(parent,fs::directory_options::skip_permission_denied,ec),end;i!=end;i.increment(ec)){
            if(ec){ec.clear();break;}
            if(i->is_directory(ec)&&sameResourceProduct(i->path().filename().string(),product))roots.push_back(i->path());
        }
    }
    if(!explicitRoot.empty()&&fs::is_directory(explicitRoot,ec))roots.insert(roots.begin(),explicitRoot);
    for(const auto& root:roots){
        addDir(root);
        for(fs::recursive_directory_iterator i(root,fs::directory_options::skip_permission_denied,ec),end;i!=end;i.increment(ec)){
            if(ec){ec.clear();break;}
            if(!i->is_regular_file(ec))continue;
            const auto name=lowerResourceName(i->path().filename().string());
            if(name=="icudtl.dat"){
                if(result.icu.empty())result.icu=i->path();
                addDir(i->path().parent_path());
            }
            if(lowerResourceName(i->path().extension().string())==".dll"){
                addDir(i->path().parent_path());
                if(name.find("platform")!=std::string::npos)result.platformLibraries.push_back(i->path());
            }
        }
    }
    return result;
}
}
