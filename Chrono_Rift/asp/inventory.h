#pragma once
#include "shared_types.h"
#include <algorithm>
#include <vector>

// ─── Inventory method implementations ────────────────────────────────────────

inline int Inventory::findContiguousFree(int need) const {
    int run = 0;
    int start = -1;
    for (int i = 0; i < INVENTORY_SLOTS; ++i) {
        if (slots[i] == WeaponID::NONE) {
            if (run == 0) start = i;
            ++run;
            if (run >= need) return start;
        } else {
            run  = 0;
            start = -1;
        }
    }
    return -1;
}

// Collect distinct weapons present in primary inventory (excluding NONE)
inline std::vector<WeaponID> getDistinctWeapons(const WeaponID* slots, int n) {
    std::vector<WeaponID> seen;
    for (int i = 0; i < n; ) {
        WeaponID w = slots[i];
        if (w != WeaponID::NONE) {
            // find if already in seen
            bool found = false;
            for (auto s : seen) if (s == w) { found = true; break; }
            if (!found) seen.push_back(w);
            // skip all slots belonging to this weapon
            int sz = getWeaponInfo(w).slot_size;
            i += sz;
        } else {
            ++i;
        }
    }
    return seen;
}

inline bool Inventory::evictForSpace(int need) {
    // Collect weapons occupying the primary inventory
    // We want to evict minimum weapons (by count) to free 'need' contiguous slots.
    // Strategy: try to create contiguous run by removing smallest weapons first
    // from the end of the array working backwards.

    // Collect weapon occupancy: (start_index, weapon_id, slot_size)
    struct OccEntry { int start; WeaponID wid; int sz; };
    std::vector<OccEntry> occs;
    for (int i = 0; i < INVENTORY_SLOTS; ) {
        WeaponID w = slots[i];
        if (w != WeaponID::NONE) {
            int sz = getWeaponInfo(w).slot_size;
            occs.push_back({i, w, sz});
            i += sz;
        } else {
            ++i;
        }
    }

    if (occs.empty()) return false;

    // Sort by size ascending so we evict smallest first
    std::sort(occs.begin(), occs.end(), [](const OccEntry& a, const OccEntry& b){
        return a.sz < b.sz;
    });

    // Evict until enough space
    int evicted = 0;
    for (auto& occ : occs) {
        if (lts_count >= MAX_LTS_WEAPONS) return false;
        // Move weapon to LTS
        lts[lts_count++] = occ.wid;
        // Clear its slots
        for (int j = occ.start; j < occ.start + occ.sz && j < INVENTORY_SLOTS; ++j)
            slots[j] = WeaponID::NONE;
        evicted += occ.sz;
        // Check if we now have enough contiguous space
        if (findContiguousFree(need) != -1) return true;
    }
    return false;
}

inline int Inventory::placeWeapon(WeaponID wid) {
    const WeaponInfo& info = getWeaponInfo(wid);
    if (info.slot_size == 0) return -1;

    int pos = findContiguousFree(info.slot_size);
    if (pos == -1) {
        // Need to evict
        if (!evictForSpace(info.slot_size)) return -1;
        pos = findContiguousFree(info.slot_size);
        if (pos == -1) return -1;
    }
    for (int i = pos; i < pos + info.slot_size; ++i)
        slots[i] = wid;
    return pos;
}

inline bool Inventory::removeWeapon(WeaponID wid) {
    const WeaponInfo& info = getWeaponInfo(wid);
    // Find first slot containing this weapon
    for (int i = 0; i < INVENTORY_SLOTS; ) {
        if (slots[i] == wid) {
            for (int j = i; j < i + info.slot_size && j < INVENTORY_SLOTS; ++j)
                slots[j] = WeaponID::NONE;
            return true;
        }
        ++i;
    }
    return false;
}

inline bool Inventory::hasWeapon(WeaponID wid) const {
    for (int i = 0; i < INVENTORY_SLOTS; ++i)
        if (slots[i] == wid) return true;
    return false;
}

inline bool Inventory::swapIn(WeaponID wid) {
    // Find in LTS
    int lts_idx = -1;
    for (int i = 0; i < lts_count; ++i) {
        if (lts[i] == wid) { lts_idx = i; break; }
    }
    if (lts_idx == -1) return false;

    // Place into primary (may evict)
    int pos = placeWeapon(wid);
    if (pos == -1) return false;

    // Remove from LTS
    lts[lts_idx] = lts[--lts_count];
    lts[lts_count] = WeaponID::NONE;
    return true;
}
