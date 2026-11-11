// RECTLIST assembly runs after guest NGG exports. It never invokes the guest
// shader for a nonexistent fourth input vertex. Each guest rectangle remains
// one logical primitive (including flat parameters, layer and primitive ID).
void Emitter::EmitRectMeshEntry(std::ostringstream& out) {
	const auto& m = input_info.vertex->mesh;
	if (m.max_vertices == 0 || m.max_primitives == 0 || m.max_vertices + m.max_primitives > 256 ||
	    m.max_primitives * 2 > 256) {
		unsupported.push_back("NGG RECTLIST expanded mesh exceeds Metal output capacity");
		return;
	}
	if (position_field.empty()) {
		unsupported.push_back("NGG RECTLIST has no position export");
		return;
	}
	bool allocation = false;
	for (auto* b: program.blocks)
		for (const auto& i: *b)
			allocation |= i.GetOpcode() == IR::ValueOpcode::MeshAllocate;
	if (!allocation) unsupported.push_back("NGG RECTLIST has no GS_ALLOC_REQ");
	out << "\nstruct KytyRectVertex {\n";
	for (const auto& f: out_fields) {
		if (f.name == "layer" || f.name == "viewport_index" || f.name.starts_with("param"))
			continue;
		out << "\t" << f.type << " " << f.name << " " << f.attribute << ";\n";
	}
	for (uint32_t loc = 0; loc < 32; ++loc)
		if (!(m.flat_parameter_mask & (1u << loc)) &&
		    out_param_field.contains(m.parameter_source[loc]))
			out << "\tfloat4 param" << loc << " [[user(locn" << loc << ")]];\n";
	out << "};\ninline KytyRectVertex kyty_rect_vertex(KytyStageOut v) { KytyRectVertex r = {};\n";
	for (const auto& f: out_fields) {
		if (f.name == "layer" || f.name == "viewport_index" || f.name.starts_with("param"))
			continue;
		if (f.name == "clip_dist") {
			for (uint32_t i = 0; i < clip_distance_count; ++i)
				out << "\tr.clip_dist[" << i << "] = v.clip_dist[" << i << "];\n";
		} else
			out << "\tr." << f.name << " = v." << f.name << ";\n";
	}
	for (uint32_t loc = 0; loc < 32; ++loc)
		if (!(m.flat_parameter_mask & (1u << loc)) &&
		    out_param_field.contains(m.parameter_source[loc]))
			out << "\tr.param" << loc << " = v." << out_param_field.at(m.parameter_source[loc])
			    << ";\n";
	out << "\treturn r; }\n"
	       "inline float4 kyty_ngg_rect_corner(float4 a, float4 b, float4 c) {\n"
	       " uint4 ua=as_type<uint4>(a),ub=as_type<uint4>(b),uc=as_type<uint4>(c);\n"
	       " return as_type<float4>(select(as_type<uint4>(b+c-a),ub,(ua==ub)&(ub==uc))); }\n"
	       "struct KytyRectPrimitive { bool culled [[primitive_culled]]; uint id "
		   "[[primitive_id]];\n";
	if (layer_output) out << "\tuint layer [[render_target_array_index]];\n";
	if (viewport_output) out << "\tuint viewport_index [[viewport_array_index]];\n";
	for (uint32_t loc = 0; loc < 32; ++loc)
		if (m.flat_parameter_mask & (1u << loc)) {
			if (!out_param_field.contains(m.parameter_source[loc])) {
				unsupported.push_back("RECTLIST flat fragment parameter has no guest export");
				continue;
			}
			out << "\tfloat4 param" << loc << " [[user(locn" << loc << ")]];\n";
		}
	out << "};\nusing KytyRectMesh=metal::mesh<KytyRectVertex,KytyRectPrimitive,"
	    << m.max_vertices + m.max_primitives << "," << m.max_primitives * 2
	    << ",metal::topology::triangle>;\n"
	       "[[mesh]] void main0(KytyRectMesh mesh,constant KytyArgumentBuffer& ab "
	       "[[buffer(0)]],constant PushData& pc_data [[buffer(1)]],"
	       "constant uint* sizes [[buffer(25)]],constant uint* mesh_draw [[buffer(26)]],device "
	       "const uchar* mesh_indices [[buffer(24)]],"
	       "uint3 tgid [[threadgroup_position_in_grid]],uint liid "
	       "[[thread_index_in_threadgroup]],uint lane [[thread_index_in_simdgroup]]) {\n";
	out << "\tthreadgroup KytyStageOut vertices[" << m.max_vertices
	    << "];\n\tthreadgroup uint allocation[2];\n"
	       "\tthreadgroup uint lds["
	    << (UsesLds() ? LdsDwords() : 1)
	    << "];\n"
	       "\tif(liid==0u){allocation[0]=0u;allocation[1]=0u;} "
	       "threadgroup_barrier(mem_flags::mem_threadgroup);\n"
	       "\tKytyStageOut out={},out_h={};uint primitive=0x80000000u,primitive_h=0x80000000u;\n"
	       "\tkyty_ngg_guest(ab,pc_data,sizes,mesh_draw,mesh_indices,tgid,liid,lane,lds,allocation,"
	       "out,out_h,primitive,primitive_h);\n"
	       "\tthreadgroup_barrier(mem_flags::mem_threadgroup|mem_flags::mem_device);\n";
	out << "\tif(allocation[0]>" << m.max_vertices << "u||allocation[1]>" << m.max_primitives
	    << "u)__builtin_trap();\n";
	for (uint32_t h = 0; h < (pair_ ? 2u : 1u); ++h) {
		out << "\t{uint i=" << (pair_ ? "((liid>>5u)<<6u)+(lane&31u)" : "liid") << (h ? "+32u" : "")
		    << ";if(i<allocation[0]){ vertices[i]=" << (h ? "out_h" : "out")
		    << ";mesh.set_vertex(i,kyty_rect_vertex(" << (h ? "out_h" : "out") << "));}}\n";
	}
	out << "\tthreadgroup_barrier(mem_flags::mem_threadgroup);\n";
	for (uint32_t h = 0; h < (pair_ ? 2u : 1u); ++h) {
		out << "\t{uint i=" << (pair_ ? "((liid>>5u)<<6u)+(lane&31u)" : "liid") << (h ? "+32u" : "")
		    << ";if(i<allocation[1]){\n"
		       "\tuint packed="
		    << (h ? "primitive_h" : "primitive")
		    << ";KytyRectPrimitive p={};p.culled=(packed&0x80000000u)!=0u;p.id=tgid.x*"
		    << m.primitives_per_group
		    << "u+i;\n"
		       "\tuint3 v=p.culled?uint3(0u):uint3(packed,packed>>10u,packed>>20u)&0x1ffu;\n"
		       "\tif(!p.culled&&any(v>=allocation[0]))__builtin_trap();\n"
		       "\tuint3 second=uint3(0u);if(!p.culled){\n"
		       "\tKytyStageOut a=vertices[v.x],b=vertices[v.y],c=vertices[v.z];\n"
		       "\tconst bool "
		       "k1=(b.position.x==a.position.x&&b.position.y==c.position.y)||(b.position.y==a."
		       "position.y&&b.position.x==c.position.x);\n"
		       "\tconst bool "
		       "k2=(c.position.x==b.position.x&&c.position.y==a.position.y)||(c.position.y==b."
		       "position.y&&c.position.x==a.position.x);\n"
		       "\tuint3 "
		       "order=k1?v.yzx:(k2?v.zxy:v);a=vertices[order.x];b=vertices[order.y];c=vertices["
		       "order.z];KytyStageOut r=b;\n";
		for (const auto& f: out_fields)
			if (f.type == "float4")
				out << "\tr." << f.name << "=kyty_ngg_rect_corner(a." << f.name << ",b." << f.name
				    << ",c." << f.name << ");\n";
		for (uint32_t c = 0; c < clip_distance_count; ++c)
			out << "\tr.clip_dist[" << c << "]=as_type<float>(a.clip_dist[" << c
			    << "] )==as_type<float>(b.clip_dist[" << c << "] )&&as_type<float>(b.clip_dist["
			    << c << "] )==as_type<float>(c.clip_dist[" << c << "] )?b.clip_dist[" << c
			    << "]:b.clip_dist[" << c << "]+c.clip_dist[" << c << "]-a.clip_dist[" << c
			    << "];\n";
		out << "\tuint "
		       "extra=allocation[0]+i;mesh.set_vertex(extra,kyty_rect_vertex(r));second=uint3("
		       "order.z,order.y,extra);\n"
		       "\tKytyStageOut provoking=vertices[v."
		    << (m.provoking_vertex ? "z" : "x") << "];\n";
		if (layer_output) out << "\tp.layer=provoking.layer;\n";
		if (viewport_output) out << "\tp.viewport_index=provoking.viewport_index;\n";
		for (uint32_t loc = 0; loc < 32; ++loc)
			if ((m.flat_parameter_mask & (1u << loc)) &&
			    out_param_field.contains(m.parameter_source[loc]))
				out << "\tp.param" << loc << "=provoking."
				    << out_param_field.at(m.parameter_source[loc]) << ";\n";
		out << "\t}\n\tmesh.set_index(i*6u,uchar(v.x));mesh.set_index(i*6u+1u,uchar(v.y));mesh.set_"
		       "index(i*6u+2u,uchar(v.z));\n"
		       "\tmesh.set_index(i*6u+3u,uchar(second.x));mesh.set_index(i*6u+4u,uchar(second.y));"
			   "mesh.set_index(i*6u+5u,uchar(second.z));\n"
		       "\tmesh.set_primitive(i*2u,p);mesh.set_primitive(i*2u+1u,p);}}\n";
	}
	out << "\tif(liid==0u)mesh.set_primitive_count(allocation[1]*2u);\n}\n";
}
