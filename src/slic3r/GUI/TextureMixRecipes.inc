// Fixed-upstream histogram/greedy ranking, adapted to Snapmaker's actual
// MixedFilamentManager and display/slicing policy. Included by TextureColorBake.
namespace Slic3r::GUI {
namespace {
struct MixTarget { Vec3f lab; float weight; };

Vec3f mix_rgb(const std::string& hex)
{
    ColorRGB color;
    if (!decode_color(hex,color)) throw std::invalid_argument("Mixed filament has an invalid display color");
    return Vec3f(color.r(),color.g(),color.b());
}

std::vector<MixTarget> texture_mix_targets(const std::vector<TextureDisplacementLayer>& layers)
{
    std::vector<std::vector<MixTarget>> per_layer;
    for (const auto& layer : layers) {
        if (!layer.color_enabled || layer.empty() || analyze_texture_detail(layer).flat_colors) continue;
        auto raw = layer; raw.smoothing = 0.f;
        const auto texture = decode_height_texture(raw);
        if (!texture.has_color()) continue;
        struct Bin { double r=0.,g=0.,b=0.; uint32_t n=0; };
        std::vector<Bin> bins(4096);
        const size_t pixels=size_t(texture.width)*size_t(texture.height);
        const size_t stride=std::max<size_t>(1,pixels/(1u<<20));
        uint64_t random=0x9E3779B97F4A7C15ull;
        size_t sampled=0;
        for (size_t start=0;start<pixels;start+=stride) {
            random=random*6364136223846793005ull+1442695040888963407ull;
            const size_t pixel=start+size_t((uint64_t(uint32_t(random>>32))*uint64_t(stride))>>32);
            if (pixel>=pixels) break;
            const auto* rgb=&texture.rgb[pixel*3];
            auto& bin=bins[size_t(rgb[0]>>4)*256+size_t(rgb[1]>>4)*16+size_t(rgb[2]>>4)];
            bin.r+=rgb[0];bin.g+=rgb[1];bin.b+=rgb[2];++bin.n;++sampled;
        }
        std::vector<MixTarget> targets;
        for (const auto& bin : bins) if (bin.n) {
            const double inverse=1./(255.*double(bin.n));
            targets.push_back({srgb_to_lab(Vec3f(float(bin.r*inverse),float(bin.g*inverse),float(bin.b*inverse))),
                               float(double(bin.n)/double(sampled))});
        }
        per_layer.push_back(std::move(targets));
    }
    std::vector<MixTarget> result;
    for (auto& layer : per_layer) for (auto& target : layer) {
        target.weight/=float(per_layer.size());result.push_back(target);
    }
    if (result.size()>256) {
        std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.weight>b.weight;});
        result.resize(256);
    }
    return result;
}

bool same_texture_mix_recipe(const MixedFilament& actual,const MixedFilament& expected)
{
    if (!actual.enabled || actual.deleted || actual.gradient_enabled || actual.pointillism_all_filaments ||
        !actual.manual_pattern.empty() || !actual.gradient_component_ids.empty() || !actual.gradient_component_weights.empty() ||
        actual.local_z_max_sublayers!=0 || actual.component_a_surface_offset!=0.f || actual.component_b_surface_offset!=0.f ||
        actual.distribution_mode!=expected.distribution_mode) return false;
    if (actual.component_a==expected.component_a && actual.component_b==expected.component_b)
        return actual.mix_b_percent==expected.mix_b_percent && actual.ratio_a==expected.ratio_a && actual.ratio_b==expected.ratio_b;
    if (actual.component_a==expected.component_b && actual.component_b==expected.component_a)
        return actual.mix_b_percent==100-expected.mix_b_percent && actual.ratio_a==expected.ratio_b && actual.ratio_b==expected.ratio_a;
    return false;
}

int find_texture_mix_recipe(const MixedFilamentManager& manager,const MixedFilament& row,size_t physical)
{
    for (size_t id=physical+1;id<=manager.total_filaments(physical);++id) {
        const auto* candidate=manager.mixed_filament_from_id(unsigned(id),physical);
        if (candidate && same_texture_mix_recipe(*candidate,row)) return int(id);
    }
    return 0;
}

int append_texture_mix_recipe(MixedFilamentManager& manager,const MixedFilament& recipe,
                              const MixedFilamentDisplayContext& context)
{
    MixedFilamentBatchEntry entry;
    entry.component_a=recipe.component_a;entry.component_b=recipe.component_b;
    entry.mix_b_percent=recipe.mix_b_percent;entry.distribution_mode=recipe.distribution_mode;
    std::vector<unsigned> ids;
    manager.add_batch_custom_filaments({entry},context.physical_colors,&ids);
    if (ids.size()!=1 || !ids[0]) throw std::runtime_error("No filament slot is available for the texture recipe");
    const int index=manager.mixed_index_from_filament_id(ids[0],context.num_physical);
    if (index<0) throw std::runtime_error("Mixed filament allocation returned an invalid slot");
    auto& row=manager.mixed_filaments()[size_t(index)];
    const auto stable_id=row.stable_id;
    row=recipe;row.stable_id=stable_id;
    row.enabled=true;row.deleted=false;row.custom=true;row.origin_auto=false;row.ui_mode=2;
    row.display_color=compute_mixed_filament_display_color(row,context);
    return int(ids[0]);
}
} // namespace

TextureMixPlan plan_texture_mix_palette(const std::vector<TextureDisplacementLayer>& layers,
    const TexturePalette& existing_palette,const MixedFilamentManager& manager,
    const MixedFilamentDisplayContext& context,TextureMixPolicy policy,int requested_count,
    const DisplacementProgressFn& progress)
{
    validate_palette(existing_palette);
    const size_t physical=context.num_physical;
    if (!physical || physical!=context.physical_colors.size() || manager.total_filaments(physical)!=existing_palette.size() ||
        existing_palette.size()>MAXIMUM_FILAMENT_NUMBER)
        throw std::invalid_argument("The texture palette does not match the physical and mixed filament slots");
    TextureMixPlan plan;
    plan.source_manager=manager;plan.preview_manager=manager;plan.context=context;plan.policy=policy;
    plan.preview_palette=existing_palette;
    plan.preview_manager.set_display_context(context);
    for (size_t i=0;i<existing_palette.size();++i) {
        if (existing_palette[i].filament_id!=int(i+1) || existing_palette[i].mixed!=(i>=physical))
            throw std::invalid_argument("The texture palette contains stale filament identities");
        if (i<physical) plan.palette.push_back(existing_palette[i]);
    }
    if (progress && !progress(0)) { plan.canceled=true;return plan; }
    if (requested_count<=0 || physical<2 || physical>=MAXIMUM_FILAMENT_NUMBER) return plan;
    const auto targets=texture_mix_targets(layers);
    if (targets.empty()) return plan;

    struct Candidate { MixedFilament row; Vec3f rgb; int reusable=0; };
    std::vector<Candidate> candidates;
    for (size_t a=1;a<=physical;++a) for (size_t b=a+1;b<=physical;++b) {
        for (int denominator=2;denominator<=6;++denominator) for (int numerator=1;numerator<denominator;++numerator) {
            if (std::gcd(numerator,denominator)!=1) continue;
            MixedFilamentBatchEntry entry;
            entry.component_a=unsigned(a);entry.component_b=unsigned(b);
            entry.mix_b_percent=100-int(std::lround(100.*numerator/denominator));
            MixedFilamentManager one;
            one.set_display_context(context);
            one.add_batch_custom_filaments({entry},context.physical_colors);
            one.apply_gradient_settings(policy.gradient_mode,float(context.preview_settings.mixed_lower_bound),
                float(context.preview_settings.mixed_upper_bound),policy.advanced_dithering);
            auto row=one.mixed_filaments().front();row.stable_id=0;
            const auto rgb=mix_rgb(compute_mixed_filament_display_color(row,context));
            candidates.push_back({row,rgb,find_texture_mix_recipe(manager,row,physical)});
        }
        if (progress && !progress(int(a*10/physical))) { plan.canceled=true;return plan; }
    }
    const auto distance=[](const Vec3f& a,const Vec3f& b){return DeltaE00(a.x(),a.y(),a.z(),b.x(),b.y(),b.z());};
    std::vector<float> pure(targets.size(),std::numeric_limits<float>::max());
    for (size_t t=0;t<targets.size();++t) for (size_t p=0;p<physical;++p)
        pure[t]=std::min(pure[t],distance(targets[t].lab,srgb_to_lab(existing_palette[p].rgb)));
    std::vector<float> distances(candidates.size()*targets.size());
    for (size_t c=0;c<candidates.size();++c) {
        const auto lab=srgb_to_lab(candidates[c].rgb);
        for (size_t t=0;t<targets.size();++t) distances[c*targets.size()+t]=distance(lab,targets[t].lab);
        if (progress && !progress(10+int(c*80/candidates.size()))) { plan.canceled=true;return plan; }
    }
    std::vector<float> current=pure;
    std::vector<bool> taken(candidates.size(),false);
    const size_t count=std::min<size_t>(size_t(std::max(0,requested_count)),MAXIMUM_FILAMENT_NUMBER-physical);
    while (plan.recipes.size()<count) {
        size_t best=candidates.size();double gain=0.;
        for (size_t c=0;c<candidates.size();++c) {
            if (taken[c] || (!candidates[c].reusable && plan.preview_manager.total_filaments(physical)>=MAXIMUM_FILAMENT_NUMBER)) continue;
            double candidate_gain=0.;
            for (size_t t=0;t<targets.size();++t) {
                const float d=distances[c*targets.size()+t];
                if (d<current[t] && d<=pure[t]-2.f) candidate_gain+=double(targets[t].weight)*(current[t]-d);
            }
            if (candidate_gain>gain) {gain=candidate_gain;best=c;}
        }
        if (best==candidates.size() || gain<.05) break;
        taken[best]=true;
        const auto& candidate=candidates[best];
        const int id=candidate.reusable ? candidate.reusable : append_texture_mix_recipe(plan.preview_manager,candidate.row,context);
        plan.recipes.push_back({candidate.row,id,candidate.reusable});
        plan.palette.push_back({id,candidate.rgb,true});
        if (!candidate.reusable) plan.preview_palette.push_back({id,candidate.rgb,true});
        for (size_t t=0;t<targets.size();++t) {
            const float d=distances[best*targets.size()+t];
            if (d<current[t] && d<=pure[t]-2.f) current[t]=d;
        }
        if (progress && !progress(90+int(plan.recipes.size()*10/count))) {plan.canceled=true;return plan;}
    }
    return plan;
}

TextureMixCommit resolve_texture_mix_commit(const TextureMixPlan& plan,TextureColorBakeResult bake)
{
    if (plan.canceled || bake.mesh.indices.empty()) throw std::runtime_error("A canceled or empty texture preview cannot be committed");
    if (bake.texture_filaments.size()!=bake.mesh.indices.size() || bake.preview_filaments.size()!=bake.mesh.indices.size())
        throw std::runtime_error("The texture bake has invalid per-face filament assignments");
    std::set<int> valid,used;
    for (const auto& entry:plan.palette) valid.insert(entry.filament_id);
    for (auto id:bake.texture_filaments) if(id) {
        if (!valid.count(id)) throw std::runtime_error("The texture bake references a filament outside its preview palette");
        used.insert(id);
    }
    const auto source_slots=plan.source_manager.total_filaments(plan.context.num_physical);
    for (size_t face=0;face<bake.preview_filaments.size();++face)
        if (bake.preview_filaments[face]>source_slots && bake.preview_filaments[face]!=bake.texture_filaments[face])
            throw std::runtime_error("Unpainted geometry references a filament outside the source palette");
    TextureMixCommit result;
    result.manager=plan.source_manager;
    result.manager.set_display_context(plan.context);
    std::map<int,int> remap;
    for (const auto& recipe:plan.recipes) if (used.count(recipe.preview_filament_id)) {
        const int existing=find_texture_mix_recipe(result.manager,recipe.row,plan.context.num_physical);
        const int actual=existing ? existing : append_texture_mix_recipe(result.manager,recipe.row,plan.context);
        remap[recipe.preview_filament_id]=actual;
        if (!existing) result.created_stable_ids.push_back(result.manager.mixed_filament_from_id(unsigned(actual),plan.context.num_physical)->stable_id);
    }
    for (auto& id:bake.texture_filaments) if (auto it=remap.find(id);it!=remap.end()) id=uint8_t(it->second);
    for (auto& id:bake.preview_filaments) if (auto it=remap.find(id);it!=remap.end()) id=uint8_t(it->second);
    result.definitions=result.manager.serialize_custom_entries();
    result.bake=std::move(bake);
    return result;
}
} // namespace Slic3r::GUI
