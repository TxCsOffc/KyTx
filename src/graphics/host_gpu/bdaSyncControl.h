#pragma once

#include "graphics/host_gpu/bdaSyncPlan.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {
uint64_t ProfileClockNs() noexcept;
uint64_t ProfileThreadId() noexcept;

class BdaSyncControl {
public:
	static const char* Path() {
		static const char* path = std::getenv("KYTY_BDA_SYNC_CONTROL_FILE");
		return path;
	}
	// On by default for UFC 5 (EXP-0011: +41.6% present rate A-B-A, 1093/1093 shadow checks
	// matched, zero mismatches). KYTY_BDA_SYNC=0 restores the full per-range scan. A launch
	// control file still drives the mode so the staged off/shadow/on experiment keeps working.
	static bool DefaultEnabled() {
		static const bool enabled = [] {
			const char* value = std::getenv("KYTY_BDA_SYNC");
			return value == nullptr || value[0] != '0';
		}();
		return enabled;
	}
	static bool Enabled() { return Path() != nullptr || DefaultEnabled(); }
	~BdaSyncControl() { if (m_initialized) { Report(ProfileClockNs()); if (m_csv) std::fclose(m_csv); } }
	template <typename Backend>
	void Run(uint64_t epoch, Backend&& backend) {
		const auto now = ProfileClockNs();
		if (!m_initialized) {
			m_initialized = true; m_window = now; m_next_report = now + 2'000'000'000ull;
			// With no control file the feature runs on its default, so start enabled rather
			// than waiting for a poll that will never find a file.
			if (Path() == nullptr && DefaultEnabled()) m_mode = Mode::On;
			if (const auto* path = std::getenv("KYTY_BDA_SYNC_CSV")) m_csv = std::fopen(path, "a");
			if (m_csv && std::ftell(m_csv) == 0) {
				std::fprintf(m_csv, "host_ns,thread,epoch_hint,mode,window_ms,calls,reference_calls,candidate_calls,comparison_calls,cpu_ms,reference_cpu_ms,candidate_cpu_ms,comparison_cpu_ms,reference_visits,planned_visits,executed_candidate_visits,full_passes,unchanged_passes,regions_considered,regions_skipped,dirty_bytes,upload_runs,upload_bytes,full_checks,matched_checks,raced_checks,mismatches,verified,rejected\n");
				std::fflush(m_csv);
			}
		}
		if (Path() != nullptr && now >= m_next_poll) {
			m_next_poll = now + 500'000'000ull;
			if (FILE* file = std::fopen(Path(), "r")) {
				char value[32] {}; std::fgets(value, sizeof(value), file); std::fclose(file);
				auto mode = Mode::Off;
				if (std::strncmp(value, "shadow", 6) == 0) mode = Mode::Shadow;
				else if (std::strncmp(value, "on", 2) == 0) mode = Mode::On;
				if (s_rejected.load(std::memory_order_relaxed)) mode = Mode::Off;
				if (mode != m_mode) {
					Report(now); m_mode = mode;
					std::fprintf(stderr, "[bda-sync] mode=%s thread=%llu\n", Name(), static_cast<unsigned long long>(ProfileThreadId()));
					std::fflush(stderr);
				}
			}
		}
		if (s_rejected.load(std::memory_order_relaxed) && m_mode != Mode::Off) { Report(now); m_mode = Mode::Off; }
		if (now >= m_next_report) Report(now);
		m_epoch = epoch;
		const auto sequence = m_mode == Mode::Off ? 0 : ++m_sequence;
		auto action = BdaSyncAction::Reference;
		if (m_mode == Mode::Shadow && (m_checks == 0 || sequence % 64 == 0)) action = BdaSyncAction::Compare;
		if (m_mode == Mode::On) {
			if (m_checks == 0 || (!m_verified && sequence % 64 == 0) || (m_verified && sequence % 512 == 0))
				action = BdaSyncAction::Compare;
			else if (m_verified) action = BdaSyncAction::Candidate;
		}
		const auto begin = ProfileClockNs();
		const auto counters = backend(action);
		const auto elapsed = ProfileClockNs() - begin;
		++m_calls; m_cpu += elapsed;
		if (action == BdaSyncAction::Reference) { ++m_reference; m_reference_cpu += elapsed; }
		else if (action == BdaSyncAction::Compare) { ++m_compared; m_compare_cpu += elapsed; }
		else { ++m_candidate; m_candidate_cpu += elapsed; m_executed_visits += counters.candidate_visits; }
		m_reference_visits += counters.reference_visits; m_planned_visits += counters.candidate_visits;
		m_full += counters.full_passes; m_unchanged += counters.unchanged_passes;
		m_regions += counters.regions_considered; m_skipped_regions += counters.regions_skipped;
		m_dirty_bytes += counters.dirty_bytes; m_upload_calls += counters.upload_calls; m_upload_bytes += counters.upload_bytes;
		if (counters.compared) {
			++m_checks; ++m_window_checks;
			if (counters.matched) {
				++m_matched; if (counters.reference_visits) m_verified = true;
			} else if (counters.raced) ++m_raced;
			else {
				++m_mismatches; s_rejected.store(true, std::memory_order_relaxed);
				std::fprintf(stderr, "[bda-sync] MISMATCH uncovered_upload_bytes=%llu; disabling candidate\n",
				    static_cast<unsigned long long>(counters.uncovered_upload_bytes)); std::fflush(stderr);
				Report(ProfileClockNs()); m_mode = Mode::Off;
			}
		}
	}
private:
	enum class Mode { Off, Shadow, On };
	const char* Name() const { return m_mode == Mode::Off ? "off" : m_mode == Mode::Shadow ? "shadow" : "on"; }
	void Report(uint64_t now) {
		if (m_csv && m_calls) {
			std::fprintf(m_csv, "%llu,%llu,%llu,%s,%.3f,%llu,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u\n",
			    static_cast<unsigned long long>(now), static_cast<unsigned long long>(ProfileThreadId()), static_cast<unsigned long long>(m_epoch), Name(), (now-m_window)/1e6,
			    (unsigned long long)m_calls, (unsigned long long)m_reference, (unsigned long long)m_candidate, (unsigned long long)m_compared,
			    m_cpu/1e6, m_reference_cpu/1e6, m_candidate_cpu/1e6, m_compare_cpu/1e6,
			    (unsigned long long)m_reference_visits, (unsigned long long)m_planned_visits, (unsigned long long)m_executed_visits,
			    (unsigned long long)m_full, (unsigned long long)m_unchanged, (unsigned long long)m_regions, (unsigned long long)m_skipped_regions,
			    (unsigned long long)m_dirty_bytes, (unsigned long long)m_upload_calls, (unsigned long long)m_upload_bytes,
			    (unsigned long long)m_window_checks, (unsigned long long)m_matched, (unsigned long long)m_raced, (unsigned long long)m_mismatches,
			    (unsigned)m_verified, (unsigned)s_rejected.load(std::memory_order_relaxed)); std::fflush(m_csv);
		}
		m_calls=m_reference=m_candidate=m_compared=m_cpu=m_reference_cpu=m_candidate_cpu=m_compare_cpu=0;
		m_reference_visits=m_planned_visits=m_executed_visits=m_full=m_unchanged=m_regions=m_skipped_regions=0;
		m_dirty_bytes=m_upload_calls=m_upload_bytes=m_window_checks=m_matched=m_raced=m_mismatches=0;
		m_window=now; m_next_report=now+2'000'000'000ull;
	}
	inline static std::atomic_bool s_rejected {false};
	Mode m_mode = Mode::Off;
	bool m_initialized=false, m_verified=false;
	FILE* m_csv=nullptr;
	uint64_t m_window=0, m_next_poll=0, m_next_report=0, m_epoch=0, m_sequence=0, m_checks=0;
	uint64_t m_calls=0,m_reference=0,m_candidate=0,m_compared=0,m_cpu=0,m_reference_cpu=0,m_candidate_cpu=0,m_compare_cpu=0;
	uint64_t m_reference_visits=0,m_planned_visits=0,m_executed_visits=0,m_full=0,m_unchanged=0,m_regions=0,m_skipped_regions=0;
	uint64_t m_dirty_bytes=0,m_upload_calls=0,m_upload_bytes=0,m_window_checks=0,m_matched=0,m_raced=0,m_mismatches=0;
};
} // namespace Libs::Graphics
