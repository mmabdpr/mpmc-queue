---------------------------- MODULE MpmcQueue ----------------------------
\* SPDX-FileCopyrightText: 2026 Mohammad Mahdi Abdollahpour <git@mohammadmahdi.com>
\* SPDX-License-Identifier: CPAL-1.0
\*
\* The contents of this file are subject to the Common Public Attribution License Version 1.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at https://opensource.org/license/cpal-1-0. The License is based on the Mozilla Public License Version 1.1 but Sections 14 and 15 have been added to cover use of software over a computer network and provide for limited attribution for the Original Developer. In addition, Exhibit A has been modified to be consistent with Exhibit B.
\* Software distributed under the License is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License for the specific language governing rights and limitations under the License.
\* The Original Code is mpmc_queue, a header-only C++20 bounded queue library.
\* The Original Developer is not separately identified and is the Initial Developer. If left blank, the Original Developer is the Initial Developer.
\* The Initial Developer of the Original Code is Mohammad Mahdi Abdollahpour. All portions of the code written by Mohammad Mahdi Abdollahpour are Copyright (c) 2026. All Rights Reserved.
\* Contributor: Mohammad Mahdi Abdollahpour.
\*
\* A small, bounded TLA+ model of the publication protocol used by
\* mpmc::detail::queue_engine.
\*
\* The queue does not read the index and the cell in one atomic operation.  Each attempt is a sequence of stages:
\*
\*   1. load a local snapshot of the index;
\*   2. load a local snapshot of the cell in the slot named by that index;
\*   3. compare-exchange using the local cell snapshot as the expected value.
\*
\* Other threads can advance the indices, consume entries and reuse slots
\* between those stages, so both the index snapshot and the cell snapshot can
\* be stale by the time the CAS runs.  The model has an explicit action for
\* every stage (PReadIndex/PLoadCell/PushCAS and
\* CReadIndex/CLoadCell/PopCAS) so TLC explores those interleavings.
\*
\* The local cell snapshot is represented by a validity bit plus the index it
\* was loaded at.  A copy of a cell equals the slot exactly until something
\* writes that slot, so every modeled slot write clears the bit for every
\* participant whose snapshot refers to that slot (see InvalidateP/InvalidateC).
\* A CAS then requires the bit to still be set.  That is the compare part of
\* compare-exchange: a stale snapshot makes the CAS action disabled,
\* as the hardware CAS would fail and the thread would reload.
\*
\* A failed CAS changes no queue state; it reloads and retries.  The model
\* therefore only has an action for a successful CAS.
\*
\* Consumption is recorded in per-position, per-consumer counters plus the
\* first value observed at each position, rather than gated on a history
\* variable.  The C++ algorithm has no "already consumed" flag: it
\* relies on the cell marker changing from full-at-position-i to
\* empty-at-position-(i + Capacity).  Accordingly, PopCAS has no
\* `consumed[i] = ...` enabling condition; instead NoDuplicateConsumption is a
\* checked invariant, so TLC would report a trace if the protocol could pop the
\* same position twice.
\*
\* Write/read index advancement is modeled as separate actions.  The queue
\* always uses two separate atomic operations (the cell CAS and the index CAS).
\* Eager mode attempts the index advance immediately after the cell CAS; lazy
\* mode leaves it to a later operation by another participant.  Modeling
\* advancement as an independent action therefore covers both modes
\* (and the window in which a thread is paused between the two atomics).
\*
\* The model is finite:
\*   - Capacity slots;
\*   - Limit publication positions (the C++ implementation has a much larger
\*     marker horizon and enforces publication_limit);
\*   - Producers and Consumers participants.
\*
\* TLC checks safety, not the C++ memory model.  See verification/README.md for
\* the scope and limitations and for the separate implementation tests.

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Producers, Consumers, Capacity, Limit

ASSUME Producers >= 1
ASSUME Consumers >= 1
ASSUME Capacity >= 1
ASSUME Limit >= Capacity

P == 1..Producers
C == 1..Consumers
Slots == 0..Capacity-1
Positions == 0..Limit-1

\* A pair with a zero producer id can never be a valid value.
None == <<0, 0>>

\* Sentinel index for a participant that has not loaded an index yet.  It can
\* never equal a valid index, so the cell-load stage cannot be skipped.
UnloadedIndex == Limit + 1

VARIABLES slots, w, r, next, published, consumed, consumedVal,
          pIndex, pValid, cIndex, cValid

vars == <<slots, w, r, next, published, consumed, consumedVal,
          pIndex, pValid, cIndex, cValid>>

\* New validity maps after a write to slot `s`: a snapshot stays valid only if
\* it was valid and refers to a different slot.
InvalidateP(s) ==
    [p \in P |-> pValid[p] /\ pIndex[p] % Capacity # s]

InvalidateC(s) ==
    [c \in C |-> cValid[c] /\ cIndex[c] % Capacity # s]

Init ==
    /\ slots = [i \in Slots |->
                     [pos |-> i, full |-> FALSE, val |-> None]]
    /\ w = 0
    /\ r = 0
    /\ next = [p \in P |-> 0]
    /\ published = [i \in Positions |-> None]
    /\ consumed = [i \in Positions |-> [c \in C |-> 0]]
    /\ consumedVal = [i \in Positions |-> None]
    /\ pIndex = [p \in P |-> UnloadedIndex]
    /\ pValid = [p \in P |-> FALSE]
    /\ cIndex = [c \in C |-> UnloadedIndex]
    /\ cValid = [c \in C |-> FALSE]

\* ------------------------------------------------------------ producer stages

\* Stage 1: snapshot the write index.  The snapshot is not refreshed until the
\* thread explicitly reloads, so it may lag the global index.  It does not load
\* the cell, so the cell snapshot is no longer valid.
PReadIndex(p) ==
    /\ pIndex' = [pIndex EXCEPT ![p] = w]
    /\ pValid' = [pValid EXCEPT ![p] = FALSE]
    /\ UNCHANGED <<slots, w, r, next, published, consumed, consumedVal,
                   cIndex, cValid>>

\* Stage 2: snapshot the cell in the slot named by the local index snapshot.
\* The snapshot is valid until the next write to that slot.
PLoadCell(p) ==
    /\ pIndex[p] <= Limit
    /\ pValid' = [pValid EXCEPT ![p] = TRUE]
    /\ UNCHANGED <<slots, w, r, next, published, consumed, consumedVal,
                   pIndex, cIndex, cValid>>

\* Stage 3: publish when the snapshotted cell is still current and empty at the
\* snapshotted position.  `pValid[p]` is the CAS compare; with a stale snapshot
\* it is false and the thread must reload.
PushCAS(p) ==
    /\ pValid[p]
    /\ pIndex[p] < Limit
    /\ slots[pIndex[p] % Capacity].pos = pIndex[p]
    /\ ~slots[pIndex[p] % Capacity].full
    /\ LET i == pIndex[p]
           s == i % Capacity
           v == <<p, next[p]>>
       IN
       /\ slots' = [slots EXCEPT
                        ![s] = [pos |-> i, full |-> TRUE, val |-> v]]
       /\ published' = [published EXCEPT ![i] = v]
       /\ next' = [next EXCEPT ![p] = next[p] + 1]
       /\ pValid' = InvalidateP(s)
       /\ cValid' = InvalidateC(s)
       /\ UNCHANGED <<w, r, consumed, consumedVal, pIndex, cIndex>>

\* Any producer may advance the write index after the cell at that position has
\* become full, or after a consumer has moved the slot past a lazy write index.
\* This is the helping step.
AdvanceWrite ==
    /\ w < Limit
    /\ \/ (slots[w % Capacity].pos = w /\
           slots[w % Capacity].full)
       \/ slots[w % Capacity].pos > w
    /\ w' = w + 1
    /\ UNCHANGED <<slots, r, next, published, consumed, consumedVal,
                   pIndex, pValid, cIndex, cValid>>

\* ------------------------------------------------------------ consumer stages

\* Stage 1: snapshot the read index; the cell snapshot is no longer valid.
CReadIndex(c) ==
    /\ cIndex' = [cIndex EXCEPT ![c] = r]
    /\ cValid' = [cValid EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<slots, w, r, next, published, consumed, consumedVal,
                   pIndex, pValid>>

\* Stage 2: snapshot the cell in the slot named by the local read snapshot.
CLoadCell(c) ==
    /\ cIndex[c] <= Limit
    /\ cValid' = [cValid EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<slots, w, r, next, published, consumed, consumedVal,
                   pIndex, pValid, cIndex>>

\* Stage 3: consume when the snapshotted cell is still current, full and at the
\* snapshotted position.  The pop copies the value out of the local snapshot;
\* while `cValid[c]` holds, that value is the current cell value.  There is no
\* history guard here: replaying a position is prevented only by the cell marker
\* transition, and NoDuplicateConsumption would catch it if it were not.  Each
\* pop increments a per-position, per-consumer counter and records the first
\* observed value, so even a repeated pop by the *same*
\* consumer (which a set of consumers would collapse) is visible to the
\* invariant checker.
PopCAS(c) ==
    /\ cValid[c]
    /\ cIndex[c] < Limit
    /\ slots[cIndex[c] % Capacity].pos = cIndex[c]
    /\ slots[cIndex[c] % Capacity].full
    /\ LET i == cIndex[c]
           s == i % Capacity
       IN
       /\ slots' = [slots EXCEPT
                        ![s] = [pos |-> i + Capacity,
                                full |-> FALSE,
                                val |-> None]]
       /\ consumed' = [consumed EXCEPT
                           ![i] = [consumed[i] EXCEPT
                                       ![c] = consumed[i][c] + 1]]
       /\ consumedVal' = [consumedVal EXCEPT
                              ![i] = IF consumedVal[i] = None
                                        THEN slots[s].val
                                        ELSE consumedVal[i]]
       /\ pValid' = InvalidateP(s)
       /\ cValid' = InvalidateC(s)
       /\ UNCHANGED <<w, r, next, published, pIndex, cIndex>>

\* Any consumer may advance the read index after the cell at that position has
\* been consumed (including when a lazy consumer left it behind).
AdvanceRead ==
    /\ r < Limit
    /\ slots[r % Capacity].pos > r
    /\ r' = r + 1
    /\ UNCHANGED <<slots, w, next, published, consumed, consumedVal,
                   pIndex, pValid, cIndex, cValid>>

Next ==
    \/ \E p \in P : PReadIndex(p)
    \/ \E p \in P : PLoadCell(p)
    \/ \E p \in P : PushCAS(p)
    \/ AdvanceWrite
    \/ \E c \in C : CReadIndex(c)
    \/ \E c \in C : CLoadCell(c)
    \/ \E c \in C : PopCAS(c)
    \/ AdvanceRead

Spec == Init /\ [][Next]_vars

\* ---------------------------------------------------------------------
\* Safety invariants
\* ---------------------------------------------------------------------

\* Every slot keeps its physical index.  A marker position may be up to
\* Capacity positions ahead of the read index because it is the next-lap
\* empty marker.
SlotIndex ==
    \A i \in Slots : slots[i].pos % Capacity = i

SlotBounds ==
    \A i \in Slots : slots[i].pos <= Limit + Capacity

\* A full cell always corresponds to a publication.  An empty cell has no
\* value.  This is the cell-level publication invariant.
PublishedCell ==
    \A i \in Slots :
        (slots[i].full => published[slots[i].pos] = slots[i].val)
        /\ (slots[i].full => slots[i].val # None)

\* A successful pop can only remove a value that was published at that
\* position.  This rules out invented values.
ConsumedWasPublished ==
    \A i \in Positions :
        consumedVal[i] # None => published[i] = consumedVal[i]

\* The property the C++ algorithm must establish without a "consumed" flag:
\* no publication position is ever popped twice, by any consumer.  This is an
\* invariant over the recorded pop counters, not an enabling guard on PopCAS.
\* The first clause catches the same consumer popping a position twice; the
\* second catches two different consumers doing so.
NoDuplicateConsumption ==
    \A i \in Positions :
        /\ \A c \in C : consumed[i][c] <= 1
        /\ Cardinality({c \in C : consumed[i][c] > 0}) <= 1

\* Publication values are unique, so a duplicated payload would also show up
\* as a duplicated position.
PublicationUnique ==
    \A i, j \in Positions :
        i # j /\ published[i] # None /\ published[j] # None
            => published[i] # published[j]

\* Publication and consumption are prefix operations in this bounded model.
PublishedPrefix ==
    \A i, j \in Positions :
        i < j /\ published[j] # None => published[i] # None

ConsumedPrefix ==
    \A i, j \in Positions :
        i < j /\ consumedVal[j] # None => consumedVal[i] # None

\* The read and write indices cannot run arbitrarily far apart.  Lazy pop
\* may leave one consumed position behind the read index while all Capacity
\* slots are full, so the write side can lead by Capacity + 1.  Lazy push
\* can leave the read side ahead of the write index by at most Capacity.
IndexWindow ==
    /\ w <= r + Capacity + 1
    /\ r <= w + Capacity

\* Occupancy never exceeds capacity.
OccupancyBound ==
    Cardinality({i \in Slots : slots[i].full}) <= Capacity

\* Local index snapshots stay inside the finite domain, and a snapshot is never
\* used before its load stage.
LocalSnapshotsBounded ==
    /\ \A p \in P : pIndex[p] <= UnloadedIndex
    /\ \A c \in C : cIndex[c] <= UnloadedIndex

Safety ==
    /\ SlotIndex
    /\ SlotBounds
    /\ PublishedCell
    /\ ConsumedWasPublished
    /\ NoDuplicateConsumption
    /\ PublicationUnique
    /\ PublishedPrefix
    /\ ConsumedPrefix
    /\ IndexWindow
    /\ OccupancyBound
    /\ LocalSnapshotsBounded
    /\ w \in 0..Limit
    /\ r \in 0..Limit

=======================================================================
