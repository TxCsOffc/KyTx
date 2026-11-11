// Native NGG execution. Guest control flow returns to a uniform workgroup epilogue;
// allocation and primitive exports are consumed, never discarded as vertex-stage hints.
void Emitter::EmitMeshEntry(std::ostringstream& out) {
	const auto& mesh = input_info.vertex->mesh;
	if (mesh.ngg_vertex &&
	    Prospero::IsRectList(static_cast<Prospero::PrimitiveType>(mesh.input_primitive))) {
		EmitRectMeshEntry(out);
		return;
	}
	if (mesh.max_vertices == 0 || mesh.max_vertices > 256 || mesh.max_primitives == 0 ||
	    mesh.max_primitives > 256) {
		unsupported.push_back("native mesh output exceeds the Metal 256 vertex/primitive contract");
		return;
	}
	bool allocation = false;
	for (const auto* block: program.blocks)
		for (const auto& inst: *block)
			allocation |= inst.GetOpcode() == IR::ValueOpcode::MeshAllocate;
	if (!allocation) unsupported.push_back("NGG program has no GS_ALLOC_REQ");
	const uint32_t width    = mesh.ngg_vertex ? mesh.InputPrimitiveSize() : 3u;
	const char*    topology = width == 1 ? "point" : width == 2 ? "line" : "triangle";
	out << "\nstruct KytyPrimitiveOut {\n"
	       "\tbool culled [[primitive_culled]];\n"
	       "\tuint id [[primitive_id]];\n";
	if (layer_output) out << "\tuint layer [[render_target_array_index]];\n";
	if (viewport_output) out << "\tuint viewport_index [[viewport_array_index]];\n";
	out << "};\nusing KytyMesh = metal::mesh<KytyStageOut, KytyPrimitiveOut, " << mesh.max_vertices
	    << ", " << mesh.max_primitives << ", metal::topology::" << topology << ">;\n";
	out << "[[mesh]] void main0(KytyMesh mesh, constant KytyArgumentBuffer& ab [[buffer(0)]], "
	       "constant PushData& pc_data [[buffer(1)]], constant uint* sizes [[buffer(25)]], "
	       "constant uint* mesh_draw [[buffer(26)]], device const uchar* mesh_indices "
		   "[[buffer(24)]], uint3 tgid [[threadgroup_position_in_grid]], "
	       "uint liid [[thread_index_in_threadgroup]], uint lane [[thread_index_in_simdgroup]]) {\n"
	       "\tthreadgroup uint allocation[2];\n"
	       "\tif (liid == 0u) { allocation[0] = 0u; allocation[1] = 0u; }\n";
	out << "\tthreadgroup uint lds[" << (UsesLds() ? LdsDwords() : 1u) << "];\n";
	if (layer_output) out << "\tthreadgroup uint layers[" << mesh.max_vertices << "];\n";
	if (viewport_output) out << "\tthreadgroup uint viewports[" << mesh.max_vertices << "];\n";
	out << "\tthreadgroup_barrier(mem_flags::mem_threadgroup);\n"
	       "\tKytyStageOut out = {}, out_h = {};\n"
	       "\tuint primitive = 0x80000000u, primitive_h = 0x80000000u;\n"
	       "\tkyty_ngg_guest(ab, pc_data, sizes, mesh_draw, mesh_indices, tgid, liid, lane, lds, "
		   "allocation, out, out_h, primitive, primitive_h);\n"
	       "\tthreadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);\n";
	out << "\tif (allocation[0] > " << mesh.max_vertices << "u || allocation[1] > "
	    << mesh.max_primitives << "u) __builtin_trap();\n";
	for (uint32_t half = 0; half < (pair_ ? 2u : 1u); ++half) {
		const char* value = half ? "out_h" : "out";
		out << "\t{ const uint index = " << (pair_ ? "((liid >> 5u) << 6u) + (lane & 31u)" : "liid")
		    << (half ? " + 32u" : "")
		    << ";\n\t\tif (index < allocation[0]) {\n"
		       "\t\t\tmesh.set_vertex(index, "
		    << value << ");\n";
		if (layer_output) out << "\t\t\tlayers[index] = " << value << ".layer;\n";
		if (viewport_output) out << "\t\t\tviewports[index] = " << value << ".viewport_index;\n";
		out << "\t\t}\n\t}\n";
	}
	out << "\tthreadgroup_barrier(mem_flags::mem_threadgroup);\n";
	for (uint32_t half = 0; half < (pair_ ? 2u : 1u); ++half) {
		out << "\t{ const uint index = " << (pair_ ? "((liid >> 5u) << 6u) + (lane & 31u)" : "liid")
		    << (half ? " + 32u" : "")
		    << ";\n\t\tif (index < allocation[1]) {\n"
		       "\t\t\tconst uint packed = "
		    << (half ? "primitive_h" : "primitive")
		    << ";\n"
		       "\t\t\tKytyPrimitiveOut p = {}; p.culled = (packed & 0x80000000u) != 0u;\n"
		       "\t\t\tp.id = tgid.x * "
		    << mesh.primitives_per_group << "u + index;\n";
		for (uint32_t c = 0; c < width; ++c) {
			// Culled NGG exports need not contain valid connectivity. The harmless zero
			// indices are only written for primitives explicitly culled by the guest.
			out << "\t\t\tconst uint v" << c << " = p.culled ? 0u : ((packed >> " << c * 10u
			    << "u) & 0x1ffu);\n"
			       "\t\t\tif (!p.culled && v"
			    << c << " >= allocation[0]) __builtin_trap();\n";
		}
		const uint32_t provoking = mesh.provoking_vertex ? width - 1u : 0u;
		// Metal flat interpolation takes the first vertex. A cyclic rotation
		// selects AMD's provoking vertex while preserving triangle winding.
		for (uint32_t c = 0; c < width; ++c)
			out << "\t\t\tmesh.set_index(index * " << width << "u + " << c << "u, uchar(v"
			    << (c + provoking) % width << "));\n";
		if (layer_output) out << "\t\t\tp.layer = p.culled ? 0u : layers[v" << provoking << "];\n";
		if (viewport_output)
			out << "\t\t\tp.viewport_index = p.culled ? 0u : viewports[v" << provoking << "];\n";
		out << "\t\t\tmesh.set_primitive(index, p);\n\t\t}\n\t}\n";
	}
	out << "\tif (liid == 0u) mesh.set_primitive_count(allocation[1]);\n}\n";
}
