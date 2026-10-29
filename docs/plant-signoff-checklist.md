# Plant sign-off checklist: Cycle Start Watch pilot

Use this to collect approvals before the **first** install. Later machines of
an approved model only need the per-machine items (section F).

**One-paragraph description for approvers:**
> A small battery-backed logger that senses whether a CNC is in cycle by
> watching the cycle start lamp through an optocoupler (an electrically
> isolated, light-based input). It is wired in parallel with the existing
> lamp and powered from the control's 24 VDC through its own fuses. It has
> **no outputs to the machine**, cannot start, stop or interlock anything,
> and requires **no PLC/ladder or parameter changes**. It stores run/stop
> times and sends them over Bluetooth Low Energy to a phone app when
> someone with the app is nearby. Removing it restores the machine to its
> original state.

---

## A. Engineering / controls

| # | Item | Who | Done |
|---|---|---|---|
| A1 | Design review of the input circuit: parallel tap, current-limited (< 10 mA normal, < 20 mA single fault), fused at the tap, no outputs. See [hardware/README.md](../hardware/README.md) | Controls engineer | |
| A2 | Confirmed: no ladder, PMC, parameter or program changes | Controls engineer | |
| A3 | Survey done for each machine model; lamp behaviour table on file (install guide step 1) | Controls engineer | |
| A4 | Load on the PMC output (+7 mA) is within its rating | Controls engineer | |
| A5 | Load on the 24 VDC supply (+30 mA) is within its spare capacity | Controls engineer | |
| A6 | Bench test plan passed and results on file ([bench-test-plan.md](bench-test-plan.md)) | Project owner | |

## B. Electrical safety / maintenance

| # | Item | Who | Done |
|---|---|---|---|
| B1 | Install by a qualified electrician under plant LOTO procedure (480 VAC in the cabinet; NFPA 70E) | Maintenance supervisor | |
| B2 | Wiring practice: ferrules, wire size, labels, 24 VDC colour code, control-wiring duct (NFPA 79 / plant standard) | Maintenance | |
| B3 | Cabinet rating kept: gland or knockout seal (NEMA 12 / IP54) | Maintenance | |
| B4 | Machine electrical drawings red-lined; plant drawing-update process followed | Maintenance / document control | |
| B5 | Removal procedure documented (install guide, "Removal") | Maintenance | |
| B6 | Equipment in or on the panel acceptable under the plant's interpretation of UL 508A / panel listing; the field-installed fused terminals are listed parts | EHS / electrical engineer | |

## C. Change management / quality

| # | Item | Who | Done |
|---|---|---|---|
| C1 | Machine modification raised through the plant's Management of Change (MOC) process | Project owner | |
| C2 | Quality confirms no effect on the product or the process (IATF 16949 change control). The device does not change machine behaviour, so normally no PPAP or customer notification is needed, but quality decides | Quality engineer | |
| C3 | Machine warranty: if the machine is under warranty or a service contract, the builder/distributor is told about the added parallel tap (written OK preferred) | Project owner | |

## D. IT / OT security (wireless)

| # | Item | Who | Done |
|---|---|---|---|
| D1 | Plant wireless policy allows BLE devices on the floor (some plants restrict any radio) | IT / OT security | |
| D2 | Explained to IT: no network connection, no Wi-Fi, BLE only; data = run/stop timestamps and a machine name; pairing with a 6-digit static passkey; the device **cannot** affect machine operation even if compromised | Project owner | |
| D3 | Phone policy: which phones (company-managed or personal) may run the app; where exported CSVs may be stored | IT | |
| D4 | Radio approval: production units use a pre-certified module (FCC/IC/CE modular approval, e.g. ESP32-C3-MINI-1); record the FCC ID | Project owner | |

## E. People

| # | Item | Who | Done |
|---|---|---|---|
| E1 | Production supervisor and union/operator reps (if applicable) told what is measured and how it will be used (machine time, not individual performance) | Production manager / HR | |
| E2 | Owner and contact for the devices named, and on the device label | Project owner | |

## F. Per machine (every install)

| # | Item | Done |
|---|---|---|
| F1 | Model is covered by A3 (survey), or a new survey was done | |
| F2 | Production informed of the downtime window | |
| F3 | Install guide followed; install record completed and signed | |
| F4 | Functional test passed, **including "cycle start still works normally" and "no alarms"** | |
| F5 | Drawings red-lined and copy left in the cabinet | |

---

## Sign-off

| Role | Name | Signature | Date |
|---|---|---|---|
| Controls engineer | | | |
| Maintenance supervisor | | | |
| EHS | | | |
| Quality | | | |
| IT / OT security | | | |
| Production manager | | | |
| Project owner | | | |
