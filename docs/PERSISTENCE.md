# Persistence and restart

## Layout

A store directory holds three files:

| File | Contents |
| --- | --- |
| `fabric.snapshot` | the last committed snapshot: one framed record whose payload is the canonical encoding of `RuntimeState` |
| `fabric.snapshot.tmp` | an in-progress snapshot; never trusted and removed on recovery |
| `fabric.journal` | append-only event records committed since the snapshot |
| `fabric.lock` | exclusive ownership of the store directory |

## Frame format

```
offset  size  field
0       4     magic 'DPUF' (little endian)
4       2     format version
6       2     semantic version
8       1     record kind (Event, Snapshot, Marker)
9       3     reserved (zero)
12      4     payload length
16      N     payload (canonical binary encoding)
16+N    32    SHA-256 of the payload
```

Lengths are checked against the record bound *before* anything is allocated, so a
hostile or corrupt length prefix cannot cause a large allocation. When the reader
runs out of bytes, the frame is reported as a torn tail rather than as a failure:
an interrupted append is expected, an interrupted record is not a record.

## Commit rules

* An accepted event is appended to the journal and forced to the device **before**
  it is applied. A crash after the append leaves the event durable and it is
  replayed; a crash before the append leaves no trace. Nothing is reported as
  applied that is not durable.
* Rotation writes the new snapshot to `fabric.snapshot.tmp`, forces it to the
  device, renames it over `fabric.snapshot`, and only then empties the journal.
  A crash at any point leaves either the previous snapshot or the new one, plus a
  temporary file that recovery deletes.
* The snapshot carries the runtime watermark, so a crash between the rename and
  the journal truncation cannot double-apply records: recovery replays only
  records newer than the snapshot's watermark.
* Recovery establishes a new durable base by checkpointing the recovered
  (conservatively fenced) state, so subsequent replays start from exactly the
  state that was recovered.
* The store directory is locked exclusively. A second runtime that tries to open
  the same store is refused with `StoreAlreadyOpen`, which is what stops two
  processes from interleaving journal records neither could replay.

## Recovery classes

`RecoveryClass` is reported, never inferred silently:

| Class | Meaning |
| --- | --- |
| `EmptyStore` | a new store: nothing to recover |
| `CleanReopen` | snapshot and journal parsed and replayed cleanly |
| `TornTailTruncated` | the last journal record was incomplete; it was dropped, the file was truncated to the last complete record, and the drop is recorded in the truncation ledger |
| `RefusedCorrupt` | structural damage: bad magic, unknown record kind, trailing bytes after a snapshot |
| `RefusedVersion` | the format or semantic version is not the one this build understands |
| `RefusedTruncated` | the journal or snapshot ends inside a record that is not at the tail |
| `RefusedIntegrity` | a declared length exceeds the bound, or a payload digest does not match |

Damage in the middle of live data is always refused. Truncating there would
silently discard acknowledged events, which this runtime will not do.

## Restart behaviour

Opening a store that contains state advances the boot incarnation and fences
everything that was proven before:

* every instance that claimed liveness moves to `Unverified` with reason
  `RestartFenced`;
* every open attempt is closed as `Cancelled`;
* every fence serial is dropped, so a pre-restart token is refused with
  `FenceMismatch`;
* every authority grant is revoked;
* health freshness is recomputed at the recovered logical instant.

The **placement projection** - declared services, groups, dependencies, policy,
generations, plans and instance placement identity - is expected to survive a
restart byte for byte, and `FabricRuntime::durable_digest()` exists to assert
exactly that. Proven liveness is expected *not* to survive, and
`FabricRuntime::state_digest()` changes accordingly. A restart never revives old
service liveness or old authority.

## Bounds and growth

The journal is bounded by `RuntimeBounds::max_journal_bytes`; when the next record
would exceed it the runtime rotates instead of failing, and rotation is
observable. Records are bounded by `max_frame_bytes`. Snapshots are bounded by the
same record bound, and a state too large to snapshot is refused rather than
partially written.
