#include "rate_timeline.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <numeric>
#include <string_view>

namespace {

bool checked_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    output = left + right;
    return true;
}

bool checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    output = left * right;
    return true;
}

bool checked_lcm(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {
    const auto divisor = std::gcd(left, right);
    return divisor != 0 && checked_multiply(left / divisor, right, output);
}

bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() >= rt::rate_domain_name_capacity) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '_' || character == ':' ||
            character == '/' || character == '@' || character == '-';
    });
}

} // namespace

namespace rt::detail {

Status compile_rate_timeline(
    std::uint32_t graph_owner,
    std::size_t phase_count,
    std::span<const PhaseHandle> compiled_order,
    std::span<const RateDomainSpec> domains,
    std::span<const RateBindingSpec> bindings,
    CompiledRatePlan& output,
    RateCompileDiagnostic& diagnostic) noexcept {
    diagnostic = {};
    if (domains.empty()) {
        if (!bindings.empty()) {
            diagnostic = {
                Status::invalid_config,
                "rate bindings require at least one rate domain"};
            return diagnostic.status;
        }
        output = {};
        return Status::ok;
    }
    if (domains.size() > rate_domain_capacity) {
        diagnostic = {
            Status::capacity_exceeded,
            "rate-domain capacity exceeded"};
        return diagnostic.status;
    }
    if (phase_count == 0 || compiled_order.size() != phase_count ||
        bindings.size() != phase_count) {
        diagnostic = {
            Status::invalid_config,
            "enabled rate model requires exactly one binding per phase"};
        return diagnostic.status;
    }

    try {
        std::vector<std::size_t> binding_by_phase(
            phase_count,
            std::numeric_limits<std::size_t>::max());
        std::vector<std::size_t> phases_per_domain(domains.size(), 0);
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            const auto& binding = bindings[index];
            if (!binding.phase.valid() ||
                binding.phase.owner() != graph_owner ||
                binding.phase.index() >= phase_count ||
                !binding.domain.valid() ||
                binding.domain.owner() != graph_owner ||
                binding.domain.index() >= domains.size() ||
                (binding.phase_kind != RatePhaseKind::cpu &&
                 binding.phase_kind != RatePhaseKind::device)) {
                diagnostic = {
                    Status::invalid_handle,
                    "rate binding contains an invalid or foreign reference"};
                return diagnostic.status;
            }
            auto& phase_binding = binding_by_phase[binding.phase.index()];
            if (phase_binding != std::numeric_limits<std::size_t>::max()) {
                diagnostic = {
                    Status::invalid_config,
                    "a phase is bound to more than one rate domain"};
                return diagnostic.status;
            }
            phase_binding = index;
            ++phases_per_domain[binding.domain.index()];
        }
        if (std::find(
                binding_by_phase.begin(),
                binding_by_phase.end(),
                std::numeric_limits<std::size_t>::max()) !=
                binding_by_phase.end() ||
            std::find(
                phases_per_domain.begin(),
                phases_per_domain.end(),
                std::size_t{0}) != phases_per_domain.end()) {
            diagnostic = {
                Status::invalid_config,
                "rate model has a missing phase or empty domain ownership"};
            return diagnostic.status;
        }

        std::uint64_t supercycle = 1;
        for (std::size_t index = 0; index < domains.size(); ++index) {
            const auto& domain = domains[index];
            if (domain.period_ns == 0 || domain.substep_count == 0 ||
                domain.substep_count > rate_domain_substep_capacity ||
                !valid_name(domain.name) ||
                (domain.criticality != RateCriticality::background &&
                 domain.criticality != RateCriticality::normal &&
                 domain.criticality != RateCriticality::critical)) {
                diagnostic = {
                    Status::invalid_config,
                    "rate domain contains invalid period, substeps, deadline, or criticality"};
                return diagnostic.status;
            }
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (domains[earlier].name == domain.name) {
                    diagnostic = {
                        Status::invalid_config,
                        "rate-domain names must be unique"};
                    return diagnostic.status;
                }
            }
            if (!checked_lcm(supercycle, domain.period_ns, supercycle)) {
                diagnostic = {
                    Status::capacity_exceeded,
                    "rate supercycle is not representable"};
                return diagnostic.status;
            }
        }

        std::uint64_t release_count = 0;
        for (std::size_t index = 0; index < domains.size(); ++index) {
            std::uint64_t domain_entries = supercycle / domains[index].period_ns;
            if (!checked_multiply(
                    domain_entries,
                    static_cast<std::uint64_t>(phases_per_domain[index]),
                    domain_entries) ||
                !checked_multiply(
                    domain_entries,
                    domains[index].substep_count,
                    domain_entries) ||
                !checked_add(release_count, domain_entries, release_count) ||
                release_count > reference_release_capacity) {
                diagnostic = {
                    Status::capacity_exceeded,
                    "reference-release capacity exceeded"};
                return diagnostic.status;
            }
        }

        CompiledRatePlan candidate;
        candidate.supercycle_ns = supercycle;
        candidate.domains.reserve(domains.size());
        candidate.bindings.reserve(phase_count);
        candidate.releases.reserve(static_cast<std::size_t>(release_count));

        const auto reference_period = domains.front().period_ns;
        for (std::size_t index = 0; index < domains.size(); ++index) {
            const auto& source = domains[index];
            CompiledRateDomain compiled;
            compiled.domain = RateDomainHandle{
                graph_owner,
                static_cast<std::uint32_t>(index)};
            std::copy(source.name.begin(), source.name.end(), compiled.name.begin());
            compiled.registration_index = index;
            compiled.period_ns = source.period_ns;
            compiled.substep_count = source.substep_count;
            compiled.relative_deadline_ns = source.relative_deadline_ns;
            compiled.budget_wcet_ns = source.budget_wcet_ns;
            compiled.criticality = source.criticality;
            compiled.optional = source.optional;
            compiled.late_action = source.late_action;
            compiled.bounded_catch_up_limit =
                source.bounded_catch_up_limit;
            compiled.releases_per_supercycle = supercycle / source.period_ns;
            const auto ratio_gcd = std::gcd(source.period_ns, reference_period);
            compiled.period_ratio_numerator = source.period_ns / ratio_gcd;
            compiled.period_ratio_denominator = reference_period / ratio_gcd;
            candidate.domains.push_back(compiled);
        }

        std::vector<std::size_t> compiled_index_by_phase(
            phase_count,
            std::numeric_limits<std::size_t>::max());
        for (std::size_t index = 0; index < compiled_order.size(); ++index) {
            const auto phase = compiled_order[index];
            if (!phase.valid() || phase.owner() != graph_owner ||
                phase.index() >= phase_count) {
                diagnostic = {
                    Status::invalid_handle,
                    "compiled phase order contains an invalid reference"};
                return diagnostic.status;
            }
            if (compiled_index_by_phase[phase.index()] !=
                std::numeric_limits<std::size_t>::max()) {
                diagnostic = {
                    Status::invalid_config,
                    "compiled phase order is not a permutation"};
                return diagnostic.status;
            }
            compiled_index_by_phase[phase.index()] = index;
            const auto& binding = bindings[binding_by_phase[phase.index()]];
            candidate.bindings.push_back({
                phase,
                binding.domain,
                binding.phase_kind,
                index,
            });
        }

        for (std::size_t domain_index = 0;
             domain_index < domains.size();
             ++domain_index) {
            const auto& domain = domains[domain_index];
            const auto sequence_count = supercycle / domain.period_ns;
            for (std::uint64_t sequence = 0;
                 sequence < sequence_count;
                 ++sequence) {
                std::uint64_t release_time = 0;
                if (!checked_multiply(sequence, domain.period_ns, release_time)) {
                    diagnostic = {
                        Status::capacity_exceeded,
                        "rate release time is not representable"};
                    return diagnostic.status;
                }
                std::uint64_t deadline_time = 0;
                if (!checked_add(
                        release_time,
                        domain.relative_deadline_ns,
                        deadline_time)) {
                    diagnostic = {
                        Status::capacity_exceeded,
                        "rate release deadline is not representable"};
                    return diagnostic.status;
                }
                for (const auto& binding : candidate.bindings) {
                    if (binding.domain.index() != domain_index) {
                        continue;
                    }
                    for (std::uint32_t substep = 0;
                         substep < domain.substep_count;
                         ++substep) {
                        candidate.releases.push_back({
                            release_time,
                            binding.domain,
                            domain_index,
                            sequence,
                            binding.phase,
                            binding.phase_kind,
                            binding.compiled_phase_index,
                            substep,
                            domain.substep_count,
                            domain.relative_deadline_ns,
                            deadline_time,
                            domain.budget_wcet_ns,
                            domain.criticality,
                            domain.optional,
                            domain.late_action,
                            domain.bounded_catch_up_limit,
                        });
                    }
                }
            }
        }
        std::sort(
            candidate.releases.begin(),
            candidate.releases.end(),
            [](const ReferenceRelease& left, const ReferenceRelease& right) {
                if (left.release_time_ns != right.release_time_ns) {
                    return left.release_time_ns < right.release_time_ns;
                }
                if (left.domain_registration_index !=
                    right.domain_registration_index) {
                    return left.domain_registration_index <
                        right.domain_registration_index;
                }
                if (left.compiled_phase_index != right.compiled_phase_index) {
                    return left.compiled_phase_index <
                        right.compiled_phase_index;
                }
                return left.substep_ordinal < right.substep_ordinal;
            });

        output = std::move(candidate);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        diagnostic = {Status::resource_exhausted, nullptr};
        return diagnostic.status;
    } catch (...) {
        diagnostic = {Status::internal_error, nullptr};
        return diagnostic.status;
    }
}

} // namespace rt::detail
