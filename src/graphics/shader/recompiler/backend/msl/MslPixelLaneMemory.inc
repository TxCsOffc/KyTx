// Scalar replacement is valid only when every LDS address is lane*4 + bank,
// banks are whole guest waves apart, and every read has a dominating write
// whose predicate includes the reader. A shared or uninitialized access makes
// the entire proof fail; it is never substituted with per-invocation scratch.
void Emitter::PreparePixelLaneStorage() {
	if (program.stage != ShaderType::Pixel || !input_info.pixel ||
	    input_info.pixel->extra_lds_size_dwords == 0)
		return;
	struct Access {
		const Inst* inst;
		uint32_t    block, order, bank;
		bool        write;
		IR::Value   predicate;
	};
	std::vector<Access> accesses;
	const uint64_t      wave_bytes = uint64_t(program.wave_size) * 4;
	std::function<std::optional<std::pair<uint64_t, uint64_t>>(IR::Value, uint32_t)> affine;
	affine = [&](IR::Value value, uint32_t depth) -> std::optional<std::pair<uint64_t, uint64_t>> {
		if (depth > 32) return std::nullopt;
		value = value.Resolve();
		if (value.IsImmediate() && value.GetType() == IR::Type::U32)
			return std::pair<uint64_t, uint64_t> {0, value.U32()};
		const auto* i = value.TryInstruction();
		if (!i) return std::nullopt;
		if (i->GetOpcode() == IR::ValueOpcode::LaneId) return std::pair<uint64_t, uint64_t> {1, 0};
		if (i->GetOpcode() == IR::ValueOpcode::IAdd32) {
			auto a = affine(i->Arg(0), depth + 1), b = affine(i->Arg(1), depth + 1);
			if (!a || !b) return std::nullopt;
			return std::pair<uint64_t, uint64_t> {a->first + b->first, a->second + b->second};
		}
		if (i->GetOpcode() == IR::ValueOpcode::ShiftLeftLogical32) {
			auto a     = affine(i->Arg(0), depth + 1);
			auto shift = ImmU32(i->Arg(1));
			if (!a || !shift || *shift > 4) return std::nullopt;
			return std::pair<uint64_t, uint64_t> {a->first << *shift, a->second << *shift};
		}
		return std::nullopt;
	};
	for (uint32_t block = 0; block < program.blocks.size(); ++block) {
		uint32_t order = 0;
		for (const auto& inst: *program.blocks[block]) {
			++order;
			if (IR::SharedAccessOf(inst.GetOpcode()) == IR::SharedAccess::None) continue;
			const auto& mem = Mem(inst);
			if (mem.kind != IR::ResourceKind::Lds) continue;
			if (inst.GetOpcode() != IR::ValueOpcode::WriteSharedU32 &&
			    inst.GetOpcode() != IR::ValueOpcode::LoadSharedU32)
				return;
			auto address = affine(inst.Arg(0), 0);
			if (!address || address->first != 4) return;
			uint64_t bank = address->second + mem.offset;
			if (bank % wave_bytes ||
			    bank + wave_bytes > uint64_t(input_info.pixel->extra_lds_size_dwords) * 4)
				return;
			const bool write = inst.GetOpcode() == IR::ValueOpcode::WriteSharedU32;
			accesses.push_back(
			    {&inst, block, order, uint32_t(bank), write, inst.Arg(write ? 2 : 1)});
		}
	}
	if (accesses.empty()) return;
	const uint32_t                 n = uint32_t(program.blocks.size());
	std::vector<std::vector<bool>> dom(n, std::vector<bool>(n, true));
	dom[0].assign(n, false);
	dom[0][0] = true;
	for (bool changed = true; changed;) {
		changed = false;
		for (uint32_t b = 1; b < n; ++b) {
			std::vector<bool> d(n, true);
			const auto        preds = program.blocks[b]->ImmPredecessors();
			if (preds.empty()) d.assign(n, false);
			for (auto* predecessor: preds) {
				uint32_t p =
				    uint32_t(std::find(program.blocks.begin(), program.blocks.end(), predecessor) -
					         program.blocks.begin());
				if (p >= n) return;
				for (uint32_t k = 0; k < n; ++k)
					d[k] = d[k] && dom[p][k];
			}
			d[b] = true;
			if (d != dom[b]) {
				dom[b]  = std::move(d);
				changed = true;
			}
		}
	}
	// SSA can retain a loop-carried EXEC predicate as phi(p, self), then
	// merge it through another phi. Resolve only trivial boolean phis with an
	// actual common incoming value. A self-only phi or a changing loop mask
	// is not evidence that a read is covered by the earlier store.
	std::function<IR::Value(IR::Value, uint32_t)> predicate_value;
	predicate_value = [&](IR::Value value, uint32_t depth) -> IR::Value {
		value = value.Resolve();
		if (depth > 32) return value;
		const auto* phi = value.TryInstruction();
		if (!phi || phi->GetOpcode() != IR::ValueOpcode::Phi || phi->GetType() != IR::Type::U1)
			return value;
		IR::Value same;
		for (size_t n = 0; n < phi->NumArgs(); ++n) {
			const auto incoming = phi->Arg(n).Resolve();
			if (incoming == value) continue;
			const auto resolved = predicate_value(incoming, depth + 1);
			if (!same.IsEmpty() && same != resolved) return value;
			same = resolved;
		}
		return same.IsEmpty() ? value : same;
	};
	std::function<bool(IR::Value, IR::Value, uint32_t)> implies;
	implies = [&](IR::Value reader, IR::Value writer, uint32_t depth) {
		if (depth > 32) return false;
		reader = predicate_value(reader, 0);
		writer = predicate_value(writer, 0);
		if (reader == writer ||
		    (writer.IsImmediate() && writer.GetType() == IR::Type::U1 && writer.U1()) ||
		    (reader.IsImmediate() && reader.GetType() == IR::Type::U1 && !reader.U1()))
			return true;
		const auto* i = reader.TryInstruction();
		return i && i->GetOpcode() == IR::ValueOpcode::LogicalAnd &&
		       (implies(i->Arg(0), writer, depth + 1) || implies(i->Arg(1), writer, depth + 1));
	};
	for (const auto& read: accesses)
		if (!read.write) {
			const bool initialized =
			    std::any_of(accesses.begin(), accesses.end(), [&](const Access& write) {
				    return write.write && write.bank == read.bank && dom[read.block][write.block] &&
					       (read.block != write.block || write.order < read.order) &&
					       implies(read.predicate, write.predicate, 0);
			    });
			if (!initialized) return;
		}
	std::map<uint32_t, uint32_t> slots;
	for (const auto& a: accesses) {
		auto [entry, inserted]  = slots.try_emplace(a.bank, uint32_t(slots.size()));
		pixel_lds_slots[a.inst] = entry->second;
	}
	pixel_lds_slot_count = uint32_t(slots.size());
	if (std::getenv("KYTY_LOG_PIXEL_LDS"))
		std::fprintf(stderr, "[PixelLDS] guest=%016llx proven_slots=%u accesses=%zu\n",
		             (unsigned long long)program.shader_hash, pixel_lds_slot_count,
		             accesses.size());
}
