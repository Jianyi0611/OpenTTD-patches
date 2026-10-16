/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file schedule_group_gui.h Hierarchical schedule entries shared by vehicle order pickers. */
#ifndef SCHEDULE_GROUP_GUI_H
#define SCHEDULE_GROUP_GUI_H

#include "order_base.h"
#include "string_func.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

struct ScheduleTreeEntry {
	const OrderList *order_list; ///< Schedule or group to display.
	uint depth; ///< Indentation depth in the schedule hierarchy.
};

/**
 * Collect the schedule folders belonging to the tree @p owner may see.
 *
 * Folders have no public flag of their own (see #AreScheduleGroupsValid), so a folder of another
 * company is part of the tree exactly when a schedule below it is visible to @p owner. Marking the
 * folders this way keeps the grouping of a public schedule visible to the other companies instead
 * of dropping the schedule at the root.
 *
 * @param owner Company viewing the schedules.
 * @return Ids of the folders to show.
 */
inline std::set<OrderListID> GetVisibleScheduleGroups(Owner owner)
{
	std::set<OrderListID> groups;
	for (const OrderList *ol : OrderList::Iterate()) {
		/* Own folders are always shown, including the empty ones. An ancestor of a player-created
		 * list always belongs to the same company (see #AreScheduleGroupsValid). */
		if (ol->IsPlayerCreated() && ol->IsScheduleGroup() && ol->GetCompany() == owner) groups.insert(ol->index);
	}
	for (const OrderList *ol : OrderList::Iterate()) {
		if (!ol->IsPlayerCreated() || ol->IsScheduleGroup() || !ol->IsVisibleToCompany(owner)) continue;
		for (OrderListID parent = ol->GetScheduleGroup(); parent != OrderListID::Invalid();) {
			const OrderList *group = OrderList::GetIfValid(parent);
			if (group == nullptr || !group->IsScheduleGroup()) break;
			/* Folders are marked from the schedule upwards, so once an ancestor is known the rest
			 * of the chain above it is marked as well. */
			if (!groups.insert(parent).second) break;
			parent = group->GetScheduleGroup();
		}
	}
	return groups;
}

/**
 * Build the visible schedule tree, grouping siblings before sorting them by name.
 * @param owner Company viewing the schedules.
 * @param matches Optional set of matching entries, including their ancestors.
 * @param collapsed Optional set of groups whose children should be hidden.
 * @return Visible groups and schedules in display order.
 */
inline std::vector<ScheduleTreeEntry> GetVisibleScheduleTree(Owner owner, const std::set<OrderListID> *matches = nullptr, const std::set<OrderListID> *collapsed = nullptr)
{
	const std::set<OrderListID> groups = GetVisibleScheduleGroups(owner);
	std::map<OrderListID, std::vector<const OrderList *>> children;
	for (const OrderList *ol : OrderList::Iterate()) {
		if (!ol->IsPlayerCreated()) continue;
		if (ol->IsScheduleGroup() ? !groups.contains(ol->index) : !ol->IsVisibleToCompany(owner)) continue;
		if (matches != nullptr && !matches->contains(ol->index)) continue;
		/* Keep the real parent as long as it is part of the tree, so a public schedule of another
		 * company stays below its folders instead of moving to the root. */
		OrderListID parent = ol->GetScheduleGroup();
		if (parent != OrderListID::Invalid() && !groups.contains(parent)) parent = OrderListID::Invalid();
		children[parent].push_back(ol);
	}
	std::vector<ScheduleTreeEntry> result;
	std::vector<ScheduleTreeEntry> pending;
	auto append = [&](OrderListID parent, uint depth) {
		auto &items = children[parent];
		std::stable_sort(items.begin(), items.end(), [](const OrderList *a, const OrderList *b) {
			if (a->IsScheduleGroup() != b->IsScheduleGroup()) return a->IsScheduleGroup();
			int order = StrNaturalCompare(a->GetName(), b->GetName());
			return order != 0 ? order < 0 : a->index < b->index;
		});
		for (auto it = items.rbegin(); it != items.rend(); ++it) pending.push_back({*it, depth});
	};
	append(OrderListID::Invalid(), 0);
	while (!pending.empty()) {
		auto item = pending.back();
		pending.pop_back();
		result.push_back(item);
		if (item.order_list->IsScheduleGroup() && (collapsed == nullptr || !collapsed->contains(item.order_list->index))) append(item.order_list->index, item.depth + 1);
	}
	return result;
}

#endif /* SCHEDULE_GROUP_GUI_H */
