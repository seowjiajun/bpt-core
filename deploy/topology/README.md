# CPU-affinity topology

Central source of truth for which bpt-core threads pin to which cores.

Each service loads the path pointed at by `topology.path` in its base TOML
(or `base.topology_path` programmatically), looks up its role names via
`bpt::common::util::Topology::core_for`, and pins hot threads at thread
launch via `bpt::common::util::pin_thread_by_role`.

An unassigned role is not an error — the service logs `role=X not in
topology — running unpinned` and continues. This makes the dev-laptop
path zero-configuration: point at `laptop.toml` (or leave `topology.path`
empty) and nothing pins.

## Files in this directory

- `laptop.toml` — dev machine. No assignments (or only cold/low-stakes
  ones). Everything runs unpinned.
- `prod-single-venue.toml` — example shape for a single-venue host
  (e.g. OKX-only trading box). Shows the expected role vocabulary.

## Role vocabulary

Roles are free-form strings but services adopt a convention:

| Role                      | Thread                                          |
|---------------------------|-------------------------------------------------|
| `mdgw.<venue>.io`         | bpt-md-gateway WS read loop per adapter         |
| `ogw.<venue>.io`          | bpt-order-gateway WS/REST IO thread per adapter |
| `ogw.poll`                | bpt-order-gateway main Aeron poll loop          |
| `strategy.main`           | bpt-strategy main poll loop                     |
| `aeron.conductor`         | Aeron MediaDriver conductor                     |
| `aeron.sender`            | Aeron MediaDriver sender                        |
| `aeron.receiver`          | Aeron MediaDriver receiver                      |

Only `mdgw.*.io` is wired up today. Others will land as the pinning
migration sweep continues; until then they sit as unassigned entries
that services log and ignore.

## Validation

At load time `Topology::load` rejects:

- core ≥ `sysconf(_SC_NPROCESSORS_ONLN)` — wrong file for this host
- duplicate core (two roles claim the same core)
- duplicate role (same role string listed twice)
- `host.total_cores` mismatch against actual nproc

Failures throw `std::runtime_error` and fail service startup — loud is
safer than silently unpinned.

## Kernel isolation

Pinning alone does **not** isolate a core from kernel noise. For a
production host, pair the topology file with kernel-cmdline options:

```
isolcpus=1-15 nohz_full=1-15 rcu_nocbs=1-15 intel_idle.max_cstate=0 processor.max_cstate=0
```

and IRQ affinity routing away from the isolated cores. Those changes
live in the host's boot/Ansible config, not here. See the production
hardening backlog for the layered tuning sequence.
