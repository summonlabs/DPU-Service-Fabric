# Proof obligations

Every obligation below is discharged by tests that run in Debug, Release and
under AddressSanitizer. Test names are `suite.test`.

## Required obligations

| Obligation | How it is proven | Tests |
| --- | --- | --- |
| Incompatible DPUs are never selected | Eligibility refuses on architecture, firmware, datapath, isolation, capacity and capability mismatch; the planner iterates only decisive candidates, and the property suite recomputes eligibility for every placed instance. | `eligibility.*`, `planner.incompatible_devices_are_never_selected`, `property.randomized_worlds_hold_every_invariant` |
| Dependency cycles and unsatisfied dependencies are explicit | A cycle is refused with `DependencyCycle` and a canonical cycle path; a requirement outside the intent is refused with `DependencyUnsatisfied`; the topological order places dependencies before dependents. | `dependency.cycles_are_detected_with_a_path`, `planner.dependency_cycles_refuse_the_intent`, `planner.unsatisfied_dependencies_are_explicit` |
| Replica accounting is exact | `placed == replicas.size()`, no service ever has more live instances than planned, and a plan that cannot place every replica is refused with no plan identity. | `planner.replica_accounting_is_exact`, `planner.anti_affinity_bounds_placement_and_refuses_when_impossible`, `property.randomized_worlds_hold_every_invariant` |
| Exclusive service scopes do not double-own authority | The registry refuses a second exclusive scope on a device with `ExclusiveScopeConflict`; the planner excludes owned devices and refuses an intent that would need two scopes on one device; the runtime refuses the second group outright. | `authority.one_exclusive_owner_per_device`, `planner.exclusive_scopes_do_not_double_own_a_device`, `lifecycle.exclusive_scope_authority_is_exclusive_in_the_runtime`, `property.randomized_worlds_hold_every_invariant` |
| Stale deployment attempts are fenced | A report with a superseded attempt number is refused; a report with a stale fence is refused; a replacement fences the previous attempt so its late report cannot verify anything. | `lifecycle.verified_effect_requires_matching_fence_and_fresh_evidence`, `lifecycle.replacement_reissues_authority_and_fences_the_old_attempt`, `adversarial.authority_cannot_be_replayed_across_a_coordinator_change` |
| DPU loss is not presented as healthy continuity without fresh evidence | Loss marks the device lost, health unknown and not fresh, fences every instance and cancels open attempts; health evidence predating the loss is refused; recovery of health requires fresh evidence and reattachment, and even then the instance is not serving until a new effect is verified. | `lifecycle.device_loss_is_never_healthy_continuity` |
| Restart cannot revive old service liveness | After a restart every instance is `Unverified`, every attempt is closed, every fence serial is dropped and every grant is revoked; a pre-restart fence is refused; the placement projection is unchanged. | `persistence.clean_reopen_preserves_the_durable_projection`, `persistence.restart_does_not_revive_liveness_or_authority`, `crash.acknowledgement_boundary_claims_no_verified_effect` |

## Additional obligations

| Obligation | How it is proven | Tests |
| --- | --- | --- |
| Accepted state is deterministic from accepted evidence and policy | Same event set, any staging order and any repetition produce the same state digest; the runtime has no wall-clock or random input to a decision. | `lifecycle.state_is_a_function_of_the_accepted_event_sequence`, `property.accepted_state_is_independent_of_staging_order`, `property.digests_are_stable_across_repeated_runs`, `concurrency.concurrently_staged_events_apply_in_canonical_order` |
| Stale or superseded evidence cannot justify current authority | Evidence is checked against its validity horizon, the instance's effect fence instant and the device's last invalidation instant, in that order, and each failure has its own code. | `eligibility.stale_evidence_makes_capability_state_unknown`, `lifecycle.device_loss_is_never_healthy_continuity`, `adversarial.stale_generations_and_authority_are_refused` |
| Missing evidence never becomes false, zero or success | Missing capability evidence yields `Unknown`/positive-refusal rather than "absent"; missing capacity yields `CapacityUnknown`; missing policy or topology refuses planning; a report without evidence is refused. | `eligibility.missing_capability_blocks_with_unknown_not_false`, `eligibility.capacity_is_unknown_without_evidence_and_exhausted_with_it`, `lifecycle.unknown_capability_blocks_placement_claims`, `lifecycle.verified_effect_requires_matching_fence_and_fresh_evidence` |
| Duplicate delivery is idempotent or explicitly fenced | A redelivered event with identical content is suppressed and changes nothing; the same origin sequence with different content is refused as conflicting; concurrent duplicate submitters apply each event exactly once. | `lifecycle.duplicate_delivery_is_idempotent_and_conflicts_are_refused`, `transport.events_submitted_across_processes_are_applied`, `concurrency.concurrent_submitters_apply_each_event_once` |
| Every bounded truncation, refusal and eviction is observable and accounted | Every drop is recorded with `requested == accepted + dropped`; the ledger is itself bounded with an overflow counter; the export and the CLI print the ledger. | `bounded.ledger_accounts_for_every_drop`, `planner.explanation_growth_is_bounded_and_accounted`, `planner.staging_bound_refuses_rather_than_dropping_stages`, `concurrency.reorder_buffer_bound_refuses_and_accounts`, `adversarial.oversized_payloads_are_refused_and_accounted` |
| Persistence round-trips all correctness-critical state without semantic loss | The durable projection digest is identical across close/reopen; the whole state round-trips through its canonical encoding; the canonical JSON export is reproducible. | `persistence.clean_reopen_preserves_the_durable_projection`, `persistence.checkpoint_compacts_the_journal`, `persistence.repeated_open_close_cycles_stay_stable`, `property.digests_are_stable_across_repeated_runs` |
| Conservative restart does not resurrect liveness or authority | Liveness is downgraded and authority revoked by construction; tests assert the downgrade, the refusal of the old fence and the absence of serving instances. | `persistence.restart_does_not_revive_liveness_or_authority`, `crash.acknowledgement_boundary_claims_no_verified_effect` |
| Malformed, corrupt, truncated or oversized input cannot produce valid-looking success | Every truncation prefix of a binary payload, every flipped byte in the identity region, hostile JSON, hostile frames, damaged journals and oversized declarations are refused with stable codes. | `adversarial.decode_refuses_every_truncation_prefix`, `adversarial.json_input_refuses_hostile_documents`, `adversarial.frames_are_bounded_and_integrity_checked`, `archive.corrupt_binary_streams_are_refused`, `persistence.mid_file_corruption_is_refused`, `persistence.oversized_declared_record_is_refused_before_allocation` |

## Crash boundaries

A real child process is hard-killed with `TerminateProcess` at each labelled
boundary, then the parent reopens the store.

| Boundary | Expected outcome | Test |
| --- | --- | --- |
| before commit | no trace: no policy, no topology, no instances, no journaled events | `crash.before_commit_leaves_no_trace` |
| after commit, before acknowledgement | the event is durable exactly once and replays to the same digest twice | `crash.after_commit_before_ack_is_durable_exactly_once` |
| after acknowledgement, before verified effect | instances exist, none is verified, none is serving, recovery reports restart fencing | `crash.acknowledgement_boundary_claims_no_verified_effect` |
| during rotation | the temporary snapshot is discarded and the previous snapshot is used | `crash.interrupted_rotation_keeps_the_previous_snapshot` |
| during shutdown | the committed state is intact | `crash.shutdown_boundary_keeps_the_committed_state` |

## Multiprocess and transport

The parent test process creates a listening socket, marks it inheritable and
spawns `dpufabric serve --inherited-socket`, then talks to it with the public
client over real TCP. Readiness needs no polling: the socket is already accepting
before the child exists, and the parent closes its own copy of the listener before
connecting.

| Property | Test |
| --- | --- |
| a child process serves the protocol over a real socket | `transport.child_process_serves_over_a_real_socket` |
| submissions and queries work across the process boundary, duplicates included | `transport.events_submitted_across_processes_are_applied` |
| a refusal is reported as a refusal | `transport.refusal_is_reported_not_faked` |
| bad magic, oversized declarations and abandoned connections are rejected while the server keeps serving | `transport.malformed_frames_are_rejected_and_the_server_survives` |
| shutdown joins every worker and leaves a valid store | `transport.shutdown_is_graceful_and_joinable` |
| shutdown wakes a connected but silent peer instead of waiting for it | `transport.shutdown_wakes_a_silent_connection` |
