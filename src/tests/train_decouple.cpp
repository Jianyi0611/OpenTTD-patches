/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file train_decouple.cpp Regression tests for read-only coupling checks and decoupling cuts. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../train.h"
#include "../cargotype.h"
#include "../tracerestrict_overtaking.h"
#include "../tracerestrict_cmd.h"
#include "../date_func.h"
#include "../train_cmd.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../core/pool_func.hpp"
#include "../station_base.h"
#include "../pathfinder/yapf/yapf.h"
#include "../pbs.h"
#include "../pathfinder/follow_track.hpp"
#include "../aircraft.h"
#include "../engine_base.h"
#include "../newgrf.h"
#include "../newgrf_spritegroup.h"
#include "../settings_type.h"
#include "../core/random_func.hpp"
#include "../core/backup_type.hpp"
#include "../map_func.h"
#include "../station_map.h"
#include "../rail_map.h"
#include "../bridge_map.h"
#include "../roadveh_transport.h"
#include "../scope.h"
#include "../gfx_type.h"
#include "../sl/saveload.h"
#include "../sl/saveload_filter.h"
#include "../strings_func.h"

extern std::string _savegame_format;
extern bool ValidateVehicleTileHash(const Vehicle *v);
extern void UpdateVehicleTileHash(Vehicle *v, bool remove);

/** Capture a real savegame to verify the network snapshot's binary representation. */
struct CoupleSnapshotCapture : SaveFilter {
	std::vector<uint8_t> bytes;
	CoupleSnapshotCapture() : SaveFilter(nullptr) {}
	void Write(uint8_t *data, size_t size) override { bytes.insert(bytes.end(), data, data + size); }
};

TEST_CASE("Aircraft - state transitions refresh callback caches even when stopped")
{
	REQUIRE(Engine::GetNumItems() == 0);
	REQUIRE(Vehicle::GetNumItems() == 0);
	REQUIRE(Engine::CanAllocateItem());
	Engine *engine = Engine::Create(VehicleType::Aircraft, 0);
	GRFFile grf;
	grf.grf_version = 8;
	engine->grf_prop.grffile = &grf;
	engine->cb36_properties_used = 0;
	SetBit(engine->cb36_properties_used, PROP_AIRCRAFT_SPEED);
	CallbackResultSpriteGroup ground(100);
	CallbackResultSpriteGroup flight(200);
	engine->grf_prop.SetSpriteGroup(CargoGRFFileProps::SG_DEFAULT, &ground);
	REQUIRE(Aircraft::CanAllocateItem(2));
	Aircraft *aircraft = Aircraft::Create();
	Aircraft *shadow = Aircraft::Create();
	aircraft->subtype = AIR_AIRCRAFT;
	shadow->subtype = AIR_SHADOW;
	aircraft->engine_type = shadow->engine_type = engine->index;
	aircraft->SetNext(shadow);
	aircraft->vehstatus.Set(VehState::Stopped);
	aircraft->cur_speed = 0;
	aircraft->state = AS_HANGAR;
	UpdateAircraftCache(aircraft);
	CHECK(aircraft->vcache.cached_max_speed == 1280);
	CHECK(aircraft->last_cached_state == AS_HANGAR);
	engine->grf_prop.SetSpriteGroup(CargoGRFFileProps::SG_DEFAULT, &flight);
	aircraft->state = AS_FLYING;
	CHECK(aircraft->Tick());
	CHECK(aircraft->vcache.cached_max_speed == 2560);
	CHECK(aircraft->last_cached_state == AS_FLYING);
	_vehicle_pool.CleanPool();
	_engine_pool.CleanPool();
}

class TrainDecoupleFixture {
protected:
	Engine *engine;

	TrainDecoupleFixture()
	{
		REQUIRE(_vehicle_pool.items == 0);
		REQUIRE(_engine_pool.items == 0);
		REQUIRE(Engine::CanAllocateItem());
		engine = Engine::Create(VehicleType::Train, 0);
	}

	~TrainDecoupleFixture()
	{
		_vehicle_pool.CleanPool();
		_engine_pool.CleanPool();
	}

	Train *MakeTrain()
	{
		REQUIRE(Train::CanAllocateItem());
		Train *v = Train::Create();
		v->SetEngine();
		v->engine_type = engine->index;
		v->gcache.cached_veh_length = VEHICLE_LENGTH;
		return v;
	}
};

TEST_CASE("Couple - early checkpoint survives order copy without changing coupling station")
{
	Order order;
	order.MakeGoToCouple();
	order.SetCoupleStation(StationID{27});
	order.SetCoupleCheckPoint(StationID{12});
	Order copy(order);
	CHECK(copy.HasCoupleCheckPoint());
	CHECK(copy.GetCoupleCheckPoint() == StationID{12});
	CHECK(copy.GetCoupleStation() == StationID{27});
	copy.SetCoupleCheckPoint(StationID::Invalid());
	CHECK_FALSE(copy.HasCoupleCheckPoint());
	CHECK(copy.GetCoupleStation() == StationID{27});
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - depot override releases checkpoint and exclusive partner")
{
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	waiting->couple_claim_cost = 42;
	moving->flags.Set({VehicleRailFlag::CoupleCheckReversePending, VehicleRailFlag::CoupleCheckReservationReleased});
	moving->reverse_distance = 16;
	CancelTrainCoupleCheckpoint(moving);
	CHECK(moving->couple_target == VehicleID::Invalid());
	CHECK(waiting->couple_claimant == VehicleID::Invalid());
	CHECK(waiting->couple_claim_cost == 0);
	CHECK(moving->reverse_distance == 0);
	CHECK_FALSE(moving->flags.Test(VehicleRailFlag::CoupleCheckReversePending));
	CHECK_FALSE(moving->flags.Test(VehicleRailFlag::CoupleCheckReservationReleased));
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - checkpoint cancellation preserves ordinary reversing and another claimant")
{
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	Train *other = MakeTrain();
	moving->couple_target = waiting->index;
	waiting->couple_claimant = other->index;
	waiting->couple_claim_cost = 42;
	moving->reverse_distance = 16;
	CancelTrainCoupleCheckpoint(moving);
	CHECK(moving->couple_target == VehicleID::Invalid());
	CHECK(waiting->couple_claimant == other->index);
	CHECK(waiting->couple_claim_cost == 42);
	CHECK(moving->reverse_distance == 16);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - diagnostics do not clear a stale claim")
{
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->current_order.MakeGoToCouple();
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	waiting->couple_claim_cost = 42;
	/* The target is no longer waiting: querying it must report the invalid
	 * claim without modifying the network snapshot from GUI scope. */
	for (int i = 0; i < 2; ++i) {
		CHECK(GetCoupleClaimant(waiting) == nullptr);
		CHECK(moving->couple_target == waiting->index);
		CHECK(waiting->couple_claimant == moving->index);
		CHECK(waiting->couple_claim_cost == 42);
	}
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Train - speed inspection preserves claim and flags until a simulation update")
{
	AutoRestoreBackup acceleration(_settings_game.vehicle.train_acceleration_model, AccelerationModel::Original);
	AutoRestoreBackup<uint8_t> braking(_settings_game.vehicle.train_braking_model, TBM_ORIGINAL);
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->current_order.MakeGoToCouple();
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	moving->flags.Set(VehicleRailFlag::ConsistSpeedReduction);
	moving->cur_speed = 100;
	const auto flags = moving->flags;
	for (int i = 0; i < 2; ++i) {
		moving->GetCurrentMaxSpeedInfo();
		CHECK(moving->couple_target == waiting->index);
		CHECK(moving->flags == flags);
		CHECK(moving->cur_speed == 100);
	}
	moving->GetCurrentMaxSpeedInfoAndUpdate();
	CHECK(moving->couple_target == VehicleID::Invalid());
	CHECK_FALSE(moving->flags.Test(VehicleRailFlag::ConsistSpeedReduction));
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - inspecting approach speed does not brake the train")
{
	AutoRestoreBackup acceleration(_settings_game.vehicle.train_acceleration_model, AccelerationModel::Original);
	AutoRestoreBackup<uint8_t> braking(_settings_game.vehicle.train_braking_model, GENERATE(TBM_ORIGINAL, TBM_REALISTIC));
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->current_order.MakeGoToCouple();
	moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	waiting->current_order.MakeWaitCouple();
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	waiting->tile = TileXY(1, 1);
	MakeRailStation(waiting->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	moving->x_pos = 0;
	waiting->x_pos = VEHICLE_LENGTH + 1;
	moving->gcache.cached_max_track_speed = 500;
	moving->cur_speed = 500;
	REQUIRE(GetCoupleClaimant(waiting) == moving);
	const auto status = moving->vehstatus;
	for (int i = 0; i < 2; ++i) {
		const auto limit = moving->GetCurrentMaxSpeedInfo();
		CHECK(limit.strict_max_speed < 500);
		CHECK(moving->cur_speed == 500);
		CHECK(moving->vehstatus == status);
	}
	moving->GetCurrentMaxSpeedInfoAndUpdate();
	CHECK(moving->cur_speed < 500);
	CHECK(moving->vehstatus.Test(VehState::TrainSlowing));
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Decouple - single unit has no cut")
{
	Train *v = MakeTrain();
	for (uint count : {0, 1, 2, 127}) CHECK(GetDecoupleVehicleForCount(v, count) == nullptr);
	CHECK(GetDecoupleCuts(v).empty());
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - network snapshot preserves wide IDs and invalid targets")
{
	AllocateMap(64, 64);
	AutoRestoreBackup format(_savegame_format, std::string{"none"});
	AutoRestoreBackup cursor(_cursor.sprites, std::vector<CursorSprite>{{0, 0, 0, 0}});
	Train *a = Train::CreateAtIndex(VehicleID{0x10001});
	Train *b = Train::CreateAtIndex(VehicleID{0x20002});
	a->engine_type = b->engine_type = engine->index;
	a->couple_target = b->index;
	a->couple_claim_cost = 123;
	a->execute_schedule_stack = {0x00120034, 0x00560078};
	b->couple_claimant = a->index;
	b->couple_body_hold = true;
	auto snapshot = std::make_shared<CoupleSnapshotCapture>();
	const SaveLoadResult result = SaveWithFilter(snapshot, false, SMF_NET_SERVER);
	DeallocateMap();
	REQUIRE(result == SaveLoadResult::Ok);
	/* Older clients must reject snapshots whose coupling criteria they cannot enforce. */
	for (const std::string field : {"current_order.couple_speed_value1", "current_order.couple_speed_value2", "current_order.couple_speed_unit1", "current_order.couple_speed_unit2"}) {
		CHECK(std::search(snapshot->bytes.begin(), snapshot->bytes.end(), field.begin(), field.end()) != snapshot->bytes.end());
	}
	const std::string speed_feature = "couple_speed_filter";
	CHECK(std::search(snapshot->bytes.begin(), snapshot->bytes.end(), speed_feature.begin(), speed_feature.end()) != snapshot->bytes.end());
	const std::array<uint8_t, 8> frames{0, 0x12, 0, 0x34, 0, 0x56, 0, 0x78};
	CHECK(std::search(snapshot->bytes.begin(), snapshot->bytes.end(), frames.begin(), frames.end()) != snapshot->bytes.end());
	const std::array<uint8_t, 4> marker{'C', 'P', 'L', 'M'};
	/* SLXI also mentions CPLM by name; the actual state chunk is the last occurrence. */
	auto chunk = std::find_end(snapshot->bytes.begin(), snapshot->bytes.end(), marker.begin(), marker.end());
	REQUIRE(chunk != snapshot->bytes.end());
	REQUIRE(std::distance(chunk, snapshot->bytes.end()) >= 46);
	auto read32 = [&](size_t offset) {
		return (uint32_t(chunk[offset]) << 24) | (uint32_t(chunk[offset + 1]) << 16) | (uint32_t(chunk[offset + 2]) << 8) | chunk[offset + 3];
	};
	CHECK(read32(4) == 38); // 4-byte count and two 17-byte records.
	CHECK(read32(8) == 2);
	CHECK(read32(12) == 0x10001);
	CHECK(read32(16) == 0x20002);
	CHECK(read32(20) == VehicleID::Invalid().base());
	CHECK(read32(24) == 123);
	CHECK(chunk[28] == 0);
	CHECK(read32(29) == 0x20002);
	CHECK(read32(33) == VehicleID::Invalid().base());
	CHECK(read32(37) == 0x10001);
	CHECK(chunk[45] == 1);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Decouple - cuts preserve legacy dual-head order counts")
{
	Train *front = MakeTrain();
	Train *rear = MakeTrain();
	Train *a = MakeTrain();
	Train *b = MakeTrain();
	front->SetMultiheaded();
	rear->SetMultiheaded();
	rear->ClearEngine();
	front->other_multiheaded_part = rear;
	rear->other_multiheaded_part = front;
	front->SetNext(rear);
	rear->SetNext(a);
	a->SetNext(b);

	const auto cuts = GetDecoupleCuts(front);
	REQUIRE(cuts.size() == 2);
	CHECK(cuts[0].num_keep == 2);
	CHECK(cuts[0].vehicle == a);
	CHECK(cuts[1].num_keep == 3);
	CHECK(cuts[1].vehicle == b);
	CHECK(GetDecoupleVehicleForCount(front, 1) == nullptr);
	for (const DecoupleCut &cut : cuts) CHECK(GetDecoupleVehicleForCount(front, cut.num_keep) == cut.vehicle);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Decouple - articulated parts stay together and primary may be behind cut")
{
	Train *head = MakeTrain();
	Train *part = MakeTrain();
	Train *engine = MakeTrain();
	head->ClearEngine();
	head->SetWagon();
	part->ClearEngine();
	part->SetArticulatedPart();
	head->SetNext(part);
	part->SetNext(engine);
	for (Train *v : {head, part, engine}) v->SetPrimary(engine);

	const auto cuts = GetDecoupleCuts(engine);
	REQUIRE(cuts.size() == 1);
	CHECK(cuts[0].num_keep == 1);
	CHECK(cuts[0].vehicle == engine);
	CHECK(GetDecoupleVehicleForCount(engine, 1) == engine);
	/* Preserve the execution path's existing clamping for large order values. */
	CHECK(GetDecoupleVehicleForCount(engine, 127) == engine);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - callback trial restores both consists on success and rejection")
{
	AutoRestoreBackup length_limit(_settings_game.vehicle.max_train_length, uint8_t{8});
	GRFFile grf;
	grf.grf_version = 8;
	engine->grf_prop.grffile = &grf;
	engine->cb36_properties_used = 0;
	SetBit(engine->cb36_properties_used, PROP_TRAIN_USER_DATA);
	CallbackResultSpriteGroup result(0x400);
	DeterministicSpriteGroup group(SpriteGroupID::Invalid());
	group.size = DSG_SIZE_DWORD;
	group.var_scope = VSG_SCOPE_SELF;
	DeterministicSpriteGroupAdjust adjust{};
	adjust.variable = 0x40;
	adjust.and_mask = UINT32_MAX;
	group.adjusts.push_back(adjust);
	group.default_group = &result;
	engine->grf_prop.SetSpriteGroup(CargoGRFFileProps::SG_DEFAULT, &group);

	Train *a = MakeTrain();
	Train *b = MakeTrain();
	Train *c = MakeTrain();
	Train *d = MakeTrain();
	a->SetNext(b);
	c->SetNext(d);
	/* Exercise a primary that is not the physical chain head. */
	a->SetPrimary(b);
	b->SetPrimary(b);
	c->SetPrimary(c);
	d->SetPrimary(c);
	c->couple_claimant = b->index;
	b->couple_target = c->index;
	c->couple_claim_cost = 123;

	for (Train *v : {a, b, c, d}) {
		v->tcache.user_def_data = 0xA5;
		v->grf_cache.position_consist_length = 12345;
		v->grf_cache.cache_valid = 1;
	}
	const NewGRFCache before = a->grf_cache;
	const uint8_t flags_before = a->vcache.cached_veh_flags;
	GameRandomSeedChecker random_checker;
	bool allowed = true;
	SECTION("accepted") {}
	SECTION("attachment rejected") {
		result.result = 0x402;
		allowed = false;
	}

	for (int attempt = 0; attempt < 2; attempt++) {
		CHECK(IsCoupleArrangementValid(b, c) == allowed);
		CHECK(a->Next() == b);
		CHECK(b->Next() == nullptr);
		CHECK(c->Next() == d);
		CHECK(d->Next() == nullptr);
		CHECK(b->First() == a);
		CHECK(d->First() == c);
		CHECK(a->Last() == b);
		CHECK(c->Last() == d);
		CHECK(a->Primary() == b);
		CHECK(d->Primary() == c);
		for (Train *v : {a, b, c, d}) {
			CHECK(v->tcache.user_def_data == 0xA5);
			CHECK(v->grf_cache == before);
			CHECK(v->vcache.cached_veh_flags == flags_before);
		}
		CHECK(c->couple_claimant == b->index);
		CHECK(b->couple_target == c->index);
		CHECK(c->couple_claim_cost == 123);
		CHECK(random_checker.Check());
	}
}

TEST_CASE("Decouple - same-direction exit bits")
{
	Order order;
	order.MakeDecouple();

	CHECK(!order.GetDecoupleFirstSameDirExit());
	CHECK(!order.GetDecoupleSecondSameDirExit());

	order.SetDecoupleFirstSameDirExit(true);
	CHECK(order.GetDecoupleFirstSameDirExit());
	CHECK(!order.GetDecoupleSecondSameDirExit());

	order.SetDecoupleSecondSameDirExit(true);
	CHECK(order.GetDecoupleFirstSameDirExit());
	CHECK(order.GetDecoupleSecondSameDirExit());

	order.SetDecoupleFirstSameDirExit(false);
	CHECK(!order.GetDecoupleFirstSameDirExit());
	CHECK(order.GetDecoupleSecondSameDirExit());

	/* The bits must not collide with the schedule IDs sharing xdata. */
	order.SetDecoupleFirstScheduleID(OrderListID{1234});
	order.SetDecoupleSecondScheduleID(OrderListID{5678});
	CHECK(order.GetDecoupleFirstScheduleID() == OrderListID{1234});
	CHECK(order.GetDecoupleSecondScheduleID() == OrderListID{5678});
	CHECK(order.GetDecoupleSecondSameDirExit());
	order.SetDecoupleFirstSameDirExit(true);
	CHECK(order.GetDecoupleFirstScheduleID() == OrderListID{1234});
	CHECK(order.GetDecoupleSecondScheduleID() == OrderListID{5678});
}

TEST_CASE("Decouple - procedural orders do not keep timetable incomplete")
{
	Order decouple;
	decouple.MakeDecouple();
	CHECK(decouple.HasNoTimetableTimes());
	CHECK(decouple.IsCompletelyTimetabled());
	Order wait;
	wait.MakeWaitCouple();
	CHECK(wait.HasNoTimetableTimes());
	CHECK(wait.IsCompletelyTimetabled());
}

TEST_CASE("Coupling wait - station target is explicit and old orders still wait in place")
{
	Order wait;
	wait.MakeWaitCouple();
	CHECK_FALSE(wait.HasWaitCoupleStation());
	CHECK(wait.GetWaitCoupleStation() == StationID::Invalid());
	wait.SetWaitCoupleStation(StationID{123});
	CHECK(wait.HasWaitCoupleStation());
	CHECK(wait.GetWaitCoupleStation() == StationID{123});
	CHECK(wait.ShouldStopAtStation(StationID::Invalid(), StationID{123}, false));
	CHECK_FALSE(wait.ShouldStopAtStation(StationID::Invalid(), StationID{124}, false));
	wait.SetWaitCoupleStation(StationID::Invalid());
	CHECK_FALSE(wait.HasWaitCoupleStation());
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Train - restore driving cab without overriding shunting policy")
{
	Backup<TrainFlipReversingAllowed> setting(_settings_game.difficulty.train_flip_reverse_allowed, TrainFlipReversingAllowed::All, FILE_LINE);
	Train *engine = MakeTrain();
	Train *wagon = MakeTrain();
	wagon->ClearEngine();
	engine->SetNext(wagon);
	engine->vehicle_flags.Set(VehicleFlag::DrivingBackwards);
	wagon->vehicle_flags.Set(VehicleFlag::DrivingBackwards);
	engine->tcache.cached_tflags |= TCF_NO_DRIVING_CAB;
	GameRandomSeedChecker random_checker;
	CHECK(engine->NeedsCabFlip());
	_settings_game.difficulty.train_flip_reverse_allowed = TrainFlipReversingAllowed::None;
	CHECK_FALSE(engine->NeedsCabFlip());
	_settings_game.difficulty.train_flip_reverse_allowed = TrainFlipReversingAllowed::EndOfLineOnly;
	CHECK_FALSE(engine->NeedsCabFlip());
	_settings_game.difficulty.train_flip_reverse_allowed = TrainFlipReversingAllowed::All;
	engine->current_order.MakeDummy();
	engine->current_order.MakeGoToCouple();
	CHECK_FALSE(engine->NeedsCabFlip());
	engine->current_order.MakeDummy();
	wagon->track = TRACK_BIT_WORMHOLE;
	CHECK_FALSE(engine->NeedsCabFlip());
	wagon->track = TRACK_BIT_X;
	CHECK(engine->NeedsCabFlip());
	CHECK(random_checker.Check());
	setting.Restore();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - allow only an adjacent engine overhang")
{
	AllocateMap(64, 64);
	Train *front = MakeTrain();
	Train *rear = MakeTrain();
	front->SetNext(rear);
	front->tile = TileXY(10, 10);
	rear->tile = TileXY(11, 10);
	front->last_station_visited = StationID{7};
	MakeRailNormal(front->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	MakeRailStation(rear->tile, OWNER_NONE, StationID{7}, Axis::X, 0, RAILTYPE_RAIL);
	CHECK_FALSE(TrainFitStation(front));
	CHECK(GetTrainCouplingStation(front) == StationID{7});
	Train *articulated = MakeTrain();
	articulated->ClearEngine();
	articulated->SetArticulatedPart();
	front->SetNext(nullptr);
	front->SetNext(articulated);
	articulated->SetNext(rear);
	articulated->tile = front->tile;
	CHECK(GetTrainCouplingStation(front) == StationID{7});
	articulated->SetWagon();
	articulated->ClearArticulatedPart();
	CHECK(GetTrainCouplingStation(front) == StationID::Invalid());
	articulated->ClearWagon();
	articulated->SetArticulatedPart();

	front->last_station_visited = StationID{8};
	CHECK(GetTrainCouplingStation(front) == StationID::Invalid());
	front->last_station_visited = StationID{7};
	front->tile = TileXY(9, 10);
	MakeRailNormal(front->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	CHECK(GetTrainCouplingStation(front) == StationID::Invalid());
	DeallocateMap();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Schedule - nested returns resume each caller")
{
	Train *v = MakeTrain();
	auto make_list = []() {
		OrderList *list = OrderList::Create();
		list->SetCompany(CompanyID{0});
		list->InitializePlayerCreated();
		for (int i = 0; i < 3; ++i) { Order order; order.MakeDummy(); list->InsertOrderAt(std::move(order), i); }
		return list;
	};
	OrderList *root = make_list();
	OrderList *parent = make_list();
	OrderList *child = make_list();
	v->orders = child;
	child->AssignVehicle(v);
	v->primary_order = root->index;
	v->primary_order_index = 2;
	v->execute_schedule_stack.push_back(parent->index.base() | (1U << 16));
	SECTION("ordinary return") {}
	SECTION("nested call at the end of another list") {
		OrderList *completed = make_list();
		v->execute_schedule_stack.push_back(completed->index.base() | (3U << 16));
	}
	v->ReturnFromExecuteSchedule();
	CHECK(v->orders == parent);
	CHECK(v->cur_real_order_index == 1);
	CHECK(v->primary_order == root->index);
	CHECK(v->IsExecutingSchedule());
	v->ReturnFromExecuteSchedule();
	CHECK(v->orders == root);
	CHECK(v->cur_real_order_index == 2);
	CHECK_FALSE(v->IsExecutingSchedule());
	_vehicle_pool.CleanPool();
	_orderlist_pool.CleanPool();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - a nested detour preserves its caller's claim")
{
	AllocateMap(64, 64);
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	OrderList *root = OrderList::Create();
	OrderList *parent = OrderList::Create();
	OrderList *child = OrderList::Create();
	for (OrderList *list : {root, parent, child}) list->SetCompany(CompanyID{0});
	Order checkpoint;
	checkpoint.MakeGoToStation(StationID{0});
	parent->GetOrderVector().push_back(std::move(checkpoint));
	Order couple;
	couple.MakeGoToCouple();
	couple.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	couple.SetCoupleCheckPoint(StationID{0});
	parent->GetOrderVector().push_back(std::move(couple));
	parent->RecalculateDerivedCounters();
	moving->orders = child;
	child->AssignVehicle(moving);
	moving->primary_order = root->index;
	moving->primary_order_index = 0;
	moving->execute_schedule_stack.push_back(parent->index.base() | (1U << 16));
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	waiting->current_order.MakeWaitCouple();
	waiting->tile = TileXY(1, 1);
	MakeRailStation(waiting->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	CHECK(GetCoupleClaimant(waiting) == moving);
	moving->execute_schedule_stack.clear();
	CHECK(GetCoupleClaimant(waiting) == nullptr);
	_vehicle_pool.CleanPool();
	_orderlist_pool.CleanPool();
	DeallocateMap();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - station inspection preserves hashes and claims")
{
	AllocateMap(64, 64);
	Train *front = MakeTrain();
	Train *part = MakeTrain();
	Train *rear = MakeTrain();
	part->ClearEngine();
	part->SetArticulatedPart();
	front->SetNext(part);
	part->SetNext(rear);
	front->tile = part->tile = TileXY(10, 10);
	rear->tile = TileXY(11, 10);
	front->last_station_visited = StationID{7};
	MakeRailNormal(front->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	MakeRailStation(rear->tile, OWNER_NONE, StationID{7}, Axis::X, 0, RAILTYPE_RAIL);
	for (Train *v : {front, part, rear}) v->UpdatePosition();
	front->couple_target = rear->index;
	rear->couple_claimant = front->index;
	const auto flags = front->flags;
	GameRandomSeedChecker random_checker;
	for (int attempt = 0; attempt < 2; ++attempt) {
		CHECK(GetTrainCouplingStation(part) == StationID{7});
		for (const Train *v : {front, part, rear}) CHECK(ValidateVehicleTileHash(v));
		CHECK(front->couple_target == rear->index);
		CHECK(rear->couple_claimant == front->index);
		CHECK(front->flags == flags);
		CHECK(random_checker.Check());
	}
	DeallocateMap();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Vehicle tile hash - carried trains stay off the track until unloaded")
{
	AllocateMap(64, 64);
	Train *carrier = MakeTrain();
	Train *carried = MakeTrain();
	carrier->tile = carried->tile = TileXY(10, 10);
	carrier->UpdatePosition();
	carried->UpdatePosition();
	REQUIRE(ValidateVehicleTileHash(carrier));
	REQUIRE(ValidateVehicleTileHash(carried));

	/* Boarding deliberately removes each part from the physical track index. */
	carried->rv_transport_flags |= Vehicle::RV_TRANSPORT_CARRIED;
	CHECK_FALSE(ValidateVehicleTileHash(carried));
	UpdateVehicleTileHash(carried, true);
	CHECK(ValidateVehicleTileHash(carried));
	CHECK(ValidateVehicleTileHash(carrier));
	CHECK(GetFirstVehicleOnTile(carrier->tile, VehicleType::Train) == carrier);

	/* Sprite/position refreshes while on board must not create a phantom train. */
	carried->tile = TileXY(12, 12);
	carried->UpdatePosition();
	CHECK(carried->hash_tile_current == INVALID_TILE);
	CHECK(GetFirstVehicleOnTile(carried->tile, VehicleType::Train) == nullptr);
	CHECK(ValidateVehicleTileHash(carried));
	CHECK(ValidateVehicleTileHash(carrier));

	/* Putting it down restores its occupancy, even on the carrier's tile. */
	carried->tile = carrier->tile;
	carried->rv_transport_flags &= ~Vehicle::RV_TRANSPORT_CARRIED;
	CHECK_FALSE(ValidateVehicleTileHash(carried));
	carried->UpdatePosition();
	CHECK(carried->hash_tile_current == carried->tile);
	CHECK(GetFirstVehicleOnTile(carried->tile, VehicleType::Train) == carried);
	CHECK(ValidateVehicleTileHash(carried));
	CHECK(ValidateVehicleTileHash(carrier));
	_vehicle_pool.CleanPool();
	DeallocateMap();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Vehicle tile hash - loading repairs stale carried state on a following part")
{
	AllocateMap(64, 64);
	Train *front = MakeTrain();
	Train *part = MakeTrain();
	front->SetNext(part);
	front->tile = part->tile = TileXY(10, 10);
	front->UpdatePosition();
	/* A following part was skipped by the initial hash rebuild because its
	 * saved carried flag disagreed with the primary's ordinary state. */
	part->rv_transport_flags |= Vehicle::RV_TRANSPORT_CARRIED;
	part->vehstatus.Set(VehState::Hidden);
	REQUIRE(ValidateVehicleTileHash(part));
	RVTransportValidateAfterLoad();
	CHECK((part->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0);
	CHECK_FALSE(part->vehstatus.Test(VehState::Hidden));
	CHECK(part->hash_tile_current == part->tile);
	CHECK(ValidateVehicleTileHash(part));
	CHECK(ValidateVehicleTileHash(front));
	_vehicle_pool.CleanPool();
	DeallocateMap();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - maximum speed filters apply to selection and existing claims")
{
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->current_order.MakeGoToCouple();
	moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	waiting->current_order.MakeWaitCouple();
	moving->vcache.cached_max_speed = GENERATE(100, 120, 140);
	waiting->vcache.cached_max_speed = 120;
	/* A stationary target and a shunting approach must still compare capabilities. */
	moving->cur_speed = 32;
	waiting->cur_speed = 0;
	moving->current_order.SetCoupleRejectSlower(GENERATE(false, true));
	moving->current_order.SetCoupleRejectFaster(GENERATE(false, true));
	const bool slower = moving->current_order.GetCoupleRejectSlower() && moving->vcache.cached_max_speed < 120;
	const bool faster = moving->current_order.GetCoupleRejectFaster() && moving->vcache.cached_max_speed > 120;
	const auto expected = slower ? CoupleCandidateResult::Slower : (faster ? CoupleCandidateResult::Faster : CoupleCandidateResult::Platform);
	waiting->tile = TileXY(1, 1);
	MakeRailNormal(waiting->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	GameRandomSeedChecker random_checker;
	/* Both routing selection and physical contact use the same speed check. */
	CHECK(GetCoupleCandidateResult(moving, moving->current_order, waiting, waiting->tile, true) == expected);
	CHECK(GetCoupleCandidateResult(moving, moving->current_order, waiting, waiting->tile, false) == expected);
	MakeRailStation(waiting->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	waiting->couple_claim_cost = 42;
	CHECK(GetCoupleClaimant(waiting) == ((slower || faster) ? nullptr : moving));
	/* Inspecting invalid claims must not mutate simulation state from GUI scope. */
	CHECK(moving->couple_target == waiting->index);
	CHECK(waiting->couple_claimant == moving->index);
	CHECK(waiting->couple_claim_cost == 42);
	CHECK(random_checker.Check());
}

TEST_CASE("Couple - speed filters survive copying independently of other order flags")
{
	Order order;
	order.MakeGoToCouple();
	CHECK_FALSE(order.GetCoupleRejectSlower());
	CHECK_FALSE(order.GetCoupleRejectFaster());
	order.SetCoupleUseWaitingSchedule(true);
	order.SetCoupleLoad(ODC_IS_EMPTY);
	order.SetCoupleRejectSlower(true);
	order.SetCoupleRejectFaster(true);
	Order copy(order);
	CHECK(copy.GetCoupleRejectSlower());
	CHECK(copy.GetCoupleRejectFaster());
	copy.SetCoupleRejectSlower(false);
	CHECK_FALSE(copy.GetCoupleRejectSlower());
	CHECK(copy.GetCoupleRejectFaster());
	CHECK(copy.GetCoupleUseWaitingSchedule());
	CHECK(copy.GetCoupleLoad() == ODC_IS_EMPTY);
	CHECK(order.GetCoupleRejectSlower());
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - speed filtering uses the order carrier of each consist")
{
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	Train *moving_front = MakeTrain();
	Train *moving = MakeTrain();
	Train *waiting_front = MakeTrain();
	Train *waiting = MakeTrain();
	moving_front->SetNext(moving);
	waiting_front->SetNext(waiting);
	moving_front->SetPrimary(moving);
	moving->SetPrimary(moving);
	waiting_front->SetPrimary(waiting);
	waiting->SetPrimary(waiting);
	moving->current_order.MakeGoToCouple();
	moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	moving->current_order.SetCoupleRejectSlower(true);
	waiting->current_order.MakeWaitCouple();
	moving->vcache.cached_max_speed = 100;
	waiting->vcache.cached_max_speed = 120;
	/* Deliberately distinct values expose accidentally inspecting a wagon's cache. */
	moving_front->vcache.cached_max_speed = 200;
	waiting_front->vcache.cached_max_speed = 50;
	CHECK(GetCoupleCandidateResult(moving_front, moving->current_order, waiting_front, TileXY(1, 1), false) == CoupleCandidateResult::Slower);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - custom speed comparisons filter candidates and claims")
{
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->current_order.MakeGoToCouple();
	moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	const uint row = GENERATE(0, 1);
	const uint8_t comparator = GENERATE(1, 2, 3, 4, 5, 6);
	const uint16_t speed = GENERATE(79, 80, 81);
	moving->current_order.SetCoupleSpeedComparator(row, comparator);
	moving->current_order.SetCoupleSpeedValue(row, 80);
	waiting->current_order.MakeWaitCouple();
	waiting->vcache.cached_max_speed = speed;
	waiting->cur_speed = 0;
	const bool accepted = comparator == 1 ? speed == 80 : comparator == 2 ? speed != 80 : comparator == 3 ? speed < 80 : comparator == 4 ? speed <= 80 : comparator == 5 ? speed > 80 : speed >= 80;
	waiting->tile = TileXY(1, 1);
	MakeRailNormal(waiting->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	GameRandomSeedChecker random_checker;
	const auto expected = accepted ? CoupleCandidateResult::Platform : CoupleCandidateResult::SpeedRange;
	CHECK(GetCoupleCandidateResult(moving, moving->current_order, waiting, waiting->tile, true) == expected);
	CHECK(GetCoupleCandidateResult(moving, moving->current_order, waiting, waiting->tile, false) == expected);
	MakeRailStation(waiting->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	CHECK(GetCoupleClaimant(waiting) == (accepted ? moving : nullptr));
	CHECK(moving->couple_target == waiting->index);
	CHECK(waiting->couple_claimant == moving->index);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - both custom speed boundaries must match and disabled rows do not filter")
{
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	waiting->tile = TileXY(1, 1);
	MakeRailNormal(waiting->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	waiting->current_order.MakeWaitCouple();
	Order order;
	order.MakeGoToCouple();
	order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	CHECK(order.GetCoupleSpeedComparator(0) == 0);
	CHECK(order.GetCoupleSpeedComparator(1) == 0);
	order.SetCoupleSpeedComparator(0, 6); // >= 80
	order.SetCoupleSpeedValue(0, 80);
	order.SetCoupleSpeedComparator(1, 4); // <= 120
	order.SetCoupleSpeedValue(1, 120);
	waiting->vcache.cached_max_speed = GENERATE(0, 79, 80, 100, 120, 121, 65535);
	bool accepted = waiting->vcache.cached_max_speed >= 80 && waiting->vcache.cached_max_speed <= 120;
	CHECK(GetCoupleCandidateResult(moving, order, waiting, waiting->tile, false) == (accepted ? CoupleCandidateResult::Platform : CoupleCandidateResult::SpeedRange));
	Order copy(order);
	CHECK(copy.GetCoupleSpeedValue(0) == 80);
	CHECK(copy.GetCoupleSpeedValue(1) == 120);
	copy.SetCoupleSpeedComparator(0, 0);
	copy.SetCoupleSpeedComparator(1, 0);
	CHECK(GetCoupleCandidateResult(moving, copy, waiting, waiting->tile, false) == CoupleCandidateResult::Platform);
	CHECK(order.GetCoupleSpeedComparator(0) == 6);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - persisted speed units ignore client locale and match displayed speeds")
{
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	AutoRestoreBackup units(_settings_game.locale.units_velocity, static_cast<uint8_t>(GENERATE(0, 1, 2, 3, 4)));
	AutoRestoreBackup new_units(_settings_newgame.locale.units_velocity, _settings_game.locale.units_velocity);
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	waiting->tile = TileXY(1, 1);
	MakeRailNormal(waiting->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	waiting->current_order.MakeWaitCouple();
	waiting->vcache.cached_max_speed = 120;
	Order order;
	order.MakeGoToCouple();
	order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	order.SetCoupleSpeedComparator(0, 1);
	for (auto [unit, value] : {std::pair{0, 75}, std::pair{1, 120}, std::pair{2, 33}, std::pair{3, 4}, std::pair{4, 65}, std::pair{5, 2}}) {
		order.SetCoupleSpeedUnit(0, unit);
		order.SetCoupleSpeedValue(0, value);
		CHECK(ConvertKmhishSpeedToIntegerDisplaySpeed(120, static_cast<uint8_t>(unit)) == value);
		CHECK(GetCoupleCandidateResult(moving, order, waiting, waiting->tile, false) == CoupleCandidateResult::Platform);
		CHECK(ConvertVelocityUnitValue(value, unit, unit) == value);
		order.SetCoupleSpeedValue(0, value + 1);
		CHECK(GetCoupleCandidateResult(moving, order, waiting, waiting->tile, false) == CoupleCandidateResult::SpeedRange);
	}
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - checkpoint speed preference is soft and preserves existing claims")
{
	AutoRestoreBackup length_limit(_settings_game.vehicle.max_train_length, uint8_t{8});
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([] { DeallocateMap(); });
	Train *moving = MakeTrain();
	Train *slow = MakeTrain();
	Train *matching = MakeTrain();
	for (Train *train : {moving, slow, matching}) train->SetFrontEngine();
	moving->tile = TileXY(10, 10);
	moving->vcache.cached_max_speed = matching->vcache.cached_max_speed = 120;
	slow->vcache.cached_max_speed = 80;
	moving->current_order.MakeGoToCouple();
	moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	for (Train *train : {slow, matching}) {
		train->current_order.MakeWaitCouple();
		train->tile = TileXY(11, 10);
		MakeRailStation(train->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	}
	CHECK(GetCoupleSpeedPreferencePenalty(moving, matching) == 0);
	CHECK(GetCoupleSpeedPreferencePenalty(moving, slow) == 10);
	GameRandomSeedChecker random_checker;
	CHECK(FindBestCoupleCheckpointTarget(moving, moving->current_order) == matching);
	/* Prefer the matching train across a moderate detour which outweighed the old score. */
	matching->tile = TileXY(19, 10);
	MakeRailStation(matching->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	CHECK(FindBestCoupleCheckpointTarget(moving, moving->current_order) == matching);
	matching->vcache.cached_max_speed = UINT16_MAX;
	CHECK(GetCoupleSpeedPreferencePenalty(moving, matching) <= 24); // Large gaps cannot impose an unbounded detour.
	matching->vcache.cached_max_speed = 120;
	matching->current_order.MakeDummy();
	CHECK(FindBestCoupleCheckpointTarget(moving, moving->current_order) == slow);
	matching->current_order.MakeWaitCouple();
	matching->tile = TileXY(40, 10);
	MakeRailStation(matching->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	CHECK(FindBestCoupleCheckpointTarget(moving, moving->current_order) == slow); // A large detour must not force a match.
	moving->couple_target = slow->index;
	slow->couple_claimant = moving->index;
	CHECK(GetCoupleClaimant(slow) == moving);
	CHECK(moving->couple_target == slow->index);
	CHECK(random_checker.Check());
}


class CouplePowerFixture : public TrainDecoupleFixture {
protected:
	AutoRestoreBackup<CompanyID> company_backup{_current_company, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<uint8_t> length_limit{_settings_game.vehicle.max_train_length, uint8_t{8}};
	AutoRestoreBackup<RailTypes> rail_direct{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->compatible_railtypes, RailTypes{RAILTYPE_RAIL}};
	AutoRestoreBackup<RailTypes> rail_power{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->powered_railtypes, RailTypes{RAILTYPE_RAIL}};
	AutoRestoreBackup<RailTypes> rail_compatibility{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->indirect_compatible_railtypes, RailTypes{RAILTYPE_RAIL}};
	Train *low;
	Train *strong;
	Train *waiting;
	Station *station;
	CouplePowerFixture()
	{
		REQUIRE(Company::GetNumItems() == 0);
		REQUIRE(Station::GetNumItems() == 0);
		ResetVehicleHash();
		AllocateMap(64, 64);
		_current_company = Company::Create()->index;
		for (uint x = 5; x <= 30; x++) MakeRailNormal(TileXY(x, 10), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		for (uint x = 5; x < 20; x++) {
			MakeRailNormal(TileXY(x, 8), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
			MakeRailNormal(TileXY(x, 12), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		}
		MakeRailNormal(TileXY(20, 8), _current_company, TRACK_BIT_RIGHT, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 9), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 10), _current_company, TRACK_BIT_X | TRACK_BIT_LEFT | TRACK_BIT_LOWER, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 11), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 12), _current_company, TRACK_BIT_UPPER, RAILTYPE_RAIL);
		station = Station::Create(TileXY(25, 10));
		station->owner = _current_company;
		station->facilities = {StationFacility::Train};
		station->train_station = TileArea(station->xy, 1, 1);
		station->rect.BeforeAddTile(station->xy, StationRect::ADD_FORCE);
		MakeRailStation(station->xy, _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
		low = MakeTrain(); strong = MakeTrain(); waiting = MakeTrain();
		Place(low, 8, 8); Place(strong, 12, 12); Place(waiting, 25);
		for (Train *train : {low, strong, waiting}) {
			train->SetFrontEngine();
			train->owner = _current_company;
			train->track = TRACK_BIT_X;
			train->direction = Direction::SW;
			train->compatible_railtypes.Set(RAILTYPE_RAIL);
			train->railtypes.Set(RAILTYPE_RAIL);
			train->vcache.cached_max_speed = 120;
			train->gcache.cached_total_length = VEHICLE_LENGTH;
			train->current_order.MakeGoToCouple();
			train->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
			train->current_order.SetCoupleSpeedBoundMode(1, CoupleSpeedBoundMode::Manual);
			train->current_order.SetCoupleSpeedValue(1, UINT16_MAX);
		}
		low->gcache.cached_power = 1000;
		strong->gcache.cached_power = 2000;
		waiting->gcache.cached_power = 0;
		waiting->current_order.MakeWaitCouple();
		waiting->last_station_visited = station->index;
	}
	~CouplePowerFixture()
	{
		_vehicle_pool.CleanPool();
		ResetVehicleHash();
		_orderlist_pool.CleanPool();
		_station_pool.CleanPool();
		_company_pool.CleanPool();
		ClearOrderDestinationRefcountMap();
		DeallocateMap();
	}
	void Place(Train *train, uint x, uint y = 10)
	{
		train->tile = TileXY(x, y);
		train->x_pos = x * TILE_SIZE + 8;
		train->y_pos = y * TILE_SIZE + 8;
		train->z_pos = 0;
		UpdateVehicleTileHash(train, false);
	}
};

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - ready stronger same-speed locomotive wins before claim")
{
	GameRandomSeedChecker random_checker;
	Train *contact = nullptr;
	uint32_t cost = 0;
	REQUIRE(YapfTrainCoupleTrack(strong, true, &contact, &cost, nullptr, waiting) != INVALID_TRACK);
	REQUIRE(contact == waiting);
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == strong);
	CHECK(strong->couple_target == waiting->index);
	CHECK(strong->current_order.IsType(OT_GOTO_COUPLE));
	CHECK(strong->cur_speed == 0);
	if (IsValidTile(strong->tile)) CHECK(GetRailReservationTrackBits(strong->tile) == TRACK_BIT_NONE);
	CHECK(random_checker.Check());
	for (Train *train : {low, strong, waiting}) CHECK(ValidateVehicleTileHash(train));
}


TEST_CASE_METHOD(CouplePowerFixture, "Couple power - unsuitable stronger locomotives never block normal claims")
{
	SECTION("query budget exhausted") {
		AutoRestoreBackup limit(_settings_game.pf.yapf.max_search_nodes, uint32_t{1});
		MakeRailNormal(TileXY(16, 12), _current_company, TRACK_BIT_X | TRACK_BIT_UPPER, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(16, 11), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		low->couple_target = waiting->index;
		ClaimCoupleTarget(low, waiting, 0);
		CHECK(GetCoupleClaimant(waiting) == low);
	}
	SECTION("fleet query budget exhausted") {
		for (uint i = 0; i < 16; i++) {
			Train *other = MakeTrain();
			Place(other, 10+i, 15);
			MakeRailNormal(other->tile, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
			other->SetFrontEngine();
			other->owner = _current_company;
			other->track = TRACK_BIT_X;
			other->direction = Direction::SW;
			other->compatible_railtypes.Set(RAILTYPE_RAIL);
			other->railtypes.Set(RAILTYPE_RAIL);
			other->vcache.cached_max_speed = 120;
			other->gcache.cached_power = 5000;
			other->current_order = strong->current_order;
		}
		low->couple_target = waiting->index;
		ClaimCoupleTarget(low, waiting, 0);
		CHECK(GetCoupleClaimant(waiting) == low);
	}
	SECTION("disconnected track") {
		MakeRailNormal(TileXY(12, 15), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		Place(strong, 12, 15);
	}
	SECTION("virtual locomotive") { strong->SetVirtual(); UpdateVehicleTileHash(strong, false); }
	SECTION("carried locomotive") { strong->rv_transport_flags |= Vehicle::RV_TRANSPORT_CARRIED; UpdateVehicleTileHash(strong, false); }
	SECTION("invalid position") { UpdateVehicleTileHash(strong, true); strong->tile = INVALID_TILE; }
	SECTION("no rail types") { strong->railtypes = {}; }
	SECTION("manually stopped") { strong->vehstatus.Set(VehState::Stopped); }
	SECTION("crashed") { strong->vehstatus.Set(VehState::Crashed); }
	SECTION("breakdown") { strong->flags.Set(VehicleRailFlag::ConsistBreakdown); }
	SECTION("reversal incomplete") { strong->reverse_distance = 16; }
	SECTION("still moving") { strong->cur_speed = 10; }
	SECTION("no power") { strong->gcache.cached_power = 0; }
	SECTION("different maximum speed") { strong->vcache.cached_max_speed = 121; }
	SECTION("order filters reject this partner") { strong->current_order.SetCoupleRejectFaster(true); waiting->vcache.cached_max_speed = 80; }
	SECTION("still loading") { strong->current_order.MakeLoading(false); }
	GameRandomSeedChecker random_checker;
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == low);
	CHECK(strong->couple_target == VehicleID::Invalid());
	if (IsValidTile(strong->tile)) CHECK(GetRailReservationTrackBits(strong->tile) == TRACK_BIT_NONE);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - protect valid claims in both directions")
{
	SECTION("do not steal a weaker locomotive's active claim") {
		low->couple_target = waiting->index;
		waiting->couple_claimant = low->index;
		REQUIRE(GetCoupleClaimant(waiting) == low);
		strong->couple_target = waiting->index;
		ClaimCoupleTarget(strong, waiting, 100);
		CHECK(GetCoupleClaimant(waiting) == low);
		CHECK(low->couple_target == waiting->index);
	}
	SECTION("do not reassign a stronger locomotive with another valid partner") {
		Train *other = MakeTrain();
		Place(other, 25, 12);
		MakeRailStation(other->tile, _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
		other->SetFrontEngine();
		other->track = TRACK_BIT_X;
		other->owner = _current_company;
		other->current_order.MakeWaitCouple();
		other->last_station_visited = station->index;
		strong->couple_target = other->index;
		other->couple_claimant = strong->index;
		REQUIRE(GetCoupleClaimant(other) == strong);
		low->couple_target = waiting->index;
		ClaimCoupleTarget(low, waiting, 0);
		CHECK(GetCoupleClaimant(waiting) == low);
		CHECK(GetCoupleClaimant(other) == strong);
		CHECK(strong->couple_target == other->index);
	}
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - equal power uses route cost then stable vehicle ID")
{
	strong->gcache.cached_power = low->gcache.cached_power;
	SECTION("closer route wins") {
		low->couple_target = waiting->index;
		ClaimCoupleTarget(low, waiting, 0);
		CHECK(GetCoupleClaimant(waiting) == strong);
	}
	SECTION("equal route costs prefer lower ID regardless of first caller") {
		Place(strong, 8, 12);
		Train *contact = nullptr;
		uint32_t low_cost = 0, strong_cost = 0;
		REQUIRE(YapfTrainCoupleTrack(low, true, &contact, &low_cost, nullptr, waiting) != INVALID_TRACK);
		REQUIRE(YapfTrainCoupleTrack(strong, true, &contact, &strong_cost, nullptr, waiting) != INVALID_TRACK);
		REQUIRE(low_cost == strong_cost);
		REQUIRE(low->index < strong->index);
		strong->couple_target = waiting->index;
		ClaimCoupleTarget(strong, waiting, strong_cost);
		CHECK(GetCoupleClaimant(waiting) == low);
	}
}


TEST_CASE_METHOD(CouplePowerFixture, "Couple power - checkpoint contenders must be ready and keep their active order")
{
	const bool ready = GENERATE(false, true);
	Station *checkpoint = Station::Create(strong->tile);
	checkpoint->owner = _current_company;
	checkpoint->facilities = {StationFacility::Train};
	checkpoint->train_station = TileArea(checkpoint->xy, 1, 1);
	checkpoint->rect.BeforeAddTile(checkpoint->xy, StationRect::ADD_FORCE);
	MakeRailStation(checkpoint->xy, _current_company, checkpoint->index, Axis::X, 0, RAILTYPE_RAIL);
	Order point;
	point.MakeGoToStation(checkpoint->index);
	Order couple(strong->current_order);
	couple.SetCoupleCheckPoint(checkpoint->index);
	strong->orders = OrderList::Create(Order(point), strong);
	strong->orders->InsertOrderAt(std::move(couple), 1);
	strong->primary_order = strong->orders->index;
	strong->current_order = point;
	strong->cur_real_order_index = strong->cur_implicit_order_index = 0;
	strong->flags.Set(VehicleRailFlag::CoupleCheckReservationReleased, ready);
	const auto before_flags = strong->flags;
	GameRandomSeedChecker random_checker;
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == (ready ? strong : low));
	CHECK(strong->current_order.IsType(OT_GOTO_STATION));
	CHECK(strong->current_order.GetDestination() == checkpoint->index);
	CHECK(strong->cur_real_order_index == 0);
	CHECK(strong->cur_implicit_order_index == 0);
	CHECK(strong->flags == before_flags);
	CHECK(strong->execute_schedule_stack.empty());
	CHECK(random_checker.Check());
	for (uint x = 5; x <= 30; x++) {
		CHECK(GetRailReservationTrackBits(TileXY(x, 10)) == TRACK_BIT_NONE);
		CHECK(GetRailReservationTrackBits(TileXY(x, 12)) == TRACK_BIT_NONE);
	}
}


TEST_CASE_METHOD(CouplePowerFixture, "Couple power - an arriving checkpoint caller still considers a ready stronger locomotive")
{
	Station *checkpoint = Station::Create(low->tile);
	checkpoint->owner = _current_company;
	checkpoint->facilities = {StationFacility::Train};
	checkpoint->train_station = TileArea(checkpoint->xy, 1, 1);
	checkpoint->rect.BeforeAddTile(checkpoint->xy, StationRect::ADD_FORCE);
	MakeRailStation(checkpoint->xy, _current_company, checkpoint->index, Axis::X, 0, RAILTYPE_RAIL);
	Order point;
	point.MakeGoToStation(checkpoint->index);
	Order couple(low->current_order);
	couple.SetCoupleCheckPoint(checkpoint->index);
	low->orders = OrderList::Create(Order(point), low);
	low->orders->InsertOrderAt(std::move(couple), 1);
	low->primary_order = low->orders->index;
	low->current_order = point;
	low->cur_speed = 50;
	low->couple_target = waiting->index;
	REQUIRE(!low->flags.Test(VehicleRailFlag::CoupleCheckReservationReleased));
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == strong);
	CHECK(low->current_order.IsType(OT_GOTO_STATION));
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - a completed reverse checkpoint remains eligible after index advance")
{
	Order point;
	point.MakeGoToStation(station->index);
	Order couple(strong->current_order);
	couple.SetCoupleCheckPoint(station->index);
	strong->orders = OrderList::Create(Order(point), strong);
	strong->orders->InsertOrderAt(Order(point), 1);
	strong->orders->InsertOrderAt(std::move(couple), 2);
	strong->primary_order = strong->orders->index;
	strong->cur_real_order_index = strong->cur_implicit_order_index = 1;
	strong->current_order = point;
	strong->flags.Set({VehicleRailFlag::CoupleCheckReversePending, VehicleRailFlag::CoupleCheckReservationReleased});
	strong->reverse_distance = GENERATE(0, 16);
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == (strong->reverse_distance == 0 ? strong : low));
	CHECK(strong->cur_real_order_index == 1);
	CHECK(strong->cur_implicit_order_index == 1);
	CHECK(strong->current_order.IsType(OT_GOTO_STATION));
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - disabled speed checks keep ordinary claims")
{
	SECTION("caller disabled") { low->current_order.SetCoupleSpeedBoundMode(1, CoupleSpeedBoundMode::Disabled); }
	SECTION("competitor disabled") { strong->current_order.SetCoupleSpeedBoundMode(1, CoupleSpeedBoundMode::Disabled); }
	GameRandomSeedChecker random_checker;
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == low);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - speed fit always precedes higher power")
{
	waiting->vcache.cached_max_speed = 80;
	low->vcache.cached_max_speed = 80;
	Train *caller = strong;
	Train *expected = low;
	SECTION("fast powerful caller yields to matching locomotive") { }
	SECTION("matching caller keeps priority") { caller = low; }
	SECTION("closer speed beats more power") { low->vcache.cached_max_speed = 90; }
	SECTION("equal gap favours not overspeeding") {
		waiting->vcache.cached_max_speed = 100;
	}
	SECTION("matching locomotive manually stopped") { low->vehstatus.Set(VehState::Stopped); expected = strong; }
	SECTION("matching locomotive unreachable") {
		Place(low, 8, 15);
		MakeRailNormal(low->tile, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		expected = strong;
	}
	SECTION("matching locomotive rejects target") {
		low->current_order.SetCoupleSpeedValue(1, 1);
		expected = strong;
	}
	SECTION("existing fast claim remains exclusive") {
		strong->couple_target = waiting->index;
		waiting->couple_claimant = strong->index;
		caller = low;
		expected = strong;
	}
	GameRandomSeedChecker random_checker;
	caller->couple_target = waiting->index;
	ClaimCoupleTarget(caller, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == expected);
	CHECK(expected->couple_target == waiting->index);
	CHECK(random_checker.Check());
	for (Train *train : {low, strong, waiting}) {
		CHECK(ValidateVehicleTileHash(train));
		CHECK(GetRailReservationTrackBits(train->tile) == TRACK_BIT_NONE);
	}
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - either speed bound enables priority")
{
	const auto mode = GENERATE(CoupleSpeedBoundMode::OwnMax, CoupleSpeedBoundMode::Manual);
	const uint row = GENERATE(0u, 1u);
	for (Train *train : {low, strong}) {
		train->current_order.SetCoupleSpeedBoundMode(0, CoupleSpeedBoundMode::Disabled);
		train->current_order.SetCoupleSpeedBoundMode(1, CoupleSpeedBoundMode::Disabled);
		train->current_order.SetCoupleSpeedBoundMode(row, mode);
		train->current_order.SetCoupleSpeedValue(row, row == 0 ? 0 : UINT16_MAX);
	}
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == strong);
}

// Existing regressions deliberately exercise their saved 25% configuration.
static constexpr uint32_t TEST_YIELD_CONFIG = 25u << 16;
class AutomaticYieldFixture : public TrainDecoupleFixture {
protected:
	AutoRestoreBackup<bool> automatic_yield_setting{_settings_game.vehicle.automatic_yield, true};
	AutoRestoreBackup<CompanyID> company_backup{_current_company, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<uint8_t> length_limit{_settings_game.vehicle.max_train_length, uint8_t{8}};
	AutoRestoreBackup<RailTypes> rail_direct{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->compatible_railtypes, RailTypes{RAILTYPE_RAIL}};
	AutoRestoreBackup<RailTypes> rail_power{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->powered_railtypes, RailTypes{RAILTYPE_RAIL}};
	AutoRestoreBackup<RailTypes> rail_indirect{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->indirect_compatible_railtypes, RailTypes{RAILTYPE_RAIL}};
	Train *slow, *medium, *fast;
	TraceRestrictSlot *membership;
	AutomaticYieldFixture(uint map_width = 64)
	{
		REQUIRE(Company::GetNumItems() == 0);
		REQUIRE(TraceRestrictSlot::GetNumItems() == 0);
		REQUIRE(_tracerestrictprogram_mapping.empty());
		ResetVehicleHash();
		AllocateMap(map_width, 64);
		_current_company = Company::Create()->index;
		membership = TraceRestrictSlot::Create(_current_company);
		membership->name = "Test yielding area";
		membership->max_occupancy = 16;
		for (uint x = 1; x <= 40; ++x) MakeRailNormal(TileXY(x, 10), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		for (uint x = 8; x <= 20; ++x) MakeRailNormal(TileXY(x, 12), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(8, 10), _current_company, TRACK_BIT_X | TRACK_BIT_RIGHT, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(8, 11), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(8, 12), _current_company, TRACK_BIT_LEFT, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 12), _current_company, TRACK_BIT_UPPER, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 11), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 10), _current_company, TRACK_BIT_X | TRACK_BIT_LOWER, RAILTYPE_RAIL);
		slow = MakeActor(7, 10, 100);
		medium = MakeActor(5, 10, 160);
		fast = MakeActor(4, 10, 200);
		Marker(3, 10, AutoYieldRole::Approach);
		Marker(10, 10, AutoYieldRole::Main);
		Marker(10, 12, AutoYieldRole::Siding);
		Marker(18, 12, AutoYieldRole::Exit);
		Marker(27, 10, AutoYieldRole::Clear);
	}
	~AutomaticYieldFixture()
	{
		ClearTraceRestrictMapping();
		_tracerestrictprogram_pool.CleanPool();
		_tracerestrictslot_pool.CleanPool();
		_vehicle_pool.CleanPool();
		ResetVehicleHash();
		_company_pool.CleanPool();
		DeallocateMap();
	}
	Train *MakeActor(uint x, uint y, uint16_t speed)
	{
		Train *v = MakeTrain();
		v->SetFrontEngine(); v->owner = _current_company;
		v->track = TRACK_BIT_X; v->direction = Direction::SW;
		v->compatible_railtypes.Set(RAILTYPE_RAIL); v->railtypes.Set(RAILTYPE_RAIL);
		v->gcache.cached_total_length = VEHICLE_LENGTH; v->vcache.cached_max_speed = speed;
		Place(v, x, y);
		return v;
	}
	void Place(Train *v, uint x, uint y)
	{
		v->tile = TileXY(x,y); v->x_pos = x * TILE_SIZE + 8; v->y_pos = y * TILE_SIZE + 8; v->z_pos = 0;
		UpdateVehicleTileHash(v, false);
	}
	TraceRestrictProgram *Marker(uint x, uint y, AutoYieldRole role, uint16_t area = INVALID_TRACE_RESTRICT_SLOT_ID.base(), uint32_t config = TEST_YIELD_CONFIG)
	{
		TileIndex tile = TileXY(x,y);
		MakeRailNormal(tile, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		SB(_m[tile].m5, 6, 2, to_underlying(RailTileType::Signals));
		SetPresentSignals(tile, SignalAlongTrackdir(TRACKDIR_X_SW));
		SetSignalType(tile, TRACK_X, SignalType::Path);
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(tile, TRACK_X), true);
		TraceRestrictInstructionItem item{0}; item.SetType(role == AutoYieldRole::Clear ? TRIT_AUTO_YIELD_CLEAR : TRIT_AUTO_YIELD_AREA);
		item.SetValue(area == INVALID_TRACE_RESTRICT_SLOT_ID.base() ? membership->index.base() : area);
		if (role != AutoYieldRole::Clear) item.SetAuxField(to_underlying(role));
		program->items = {item.AsProgramItem()};
		if (role != AutoYieldRole::Clear) program->items.emplace_back(config);
		REQUIRE(program->Validate().Succeeded());
		InvalidateAutoYieldAreas();
		UpdateAutoYieldAreas();
		return program;
	}
	TraceRestrictSlot *Resource(uint32_t ref)
	{
		if (ref == AUTO_YIELD_DETECTION_REF) return membership;
		for (TraceRestrictSlot *slot : TraceRestrictSlot::Iterate()) if (slot->automatic_yield_area == membership->index.base() + 1 && slot->automatic_yield_signal == ref) return slot;
		return nullptr;
	}
	TraceRestrictProgramResult Execute(Train *v, uint x, uint y, TraceRestrictProgramInputSlotPermissions permissions = TRPISP_NONE)
	{
		TraceRestrictProgramInput input(TileXY(x,y), TRACKDIR_X_SW, nullptr, nullptr);
		input.permitted_slot_operations = permissions;
		TraceRestrictProgramResult out;
		GetExistingTraceRestrictProgram(input.tile, TRACK_X)->Execute(v, input, out);
		return out;
	}
	void Register(Train *v) { Execute(v, 3, 10, TRPISP_RELEASE_FRONT); }
	void BackSignal(uint x, SignalType type)
	{
		TileIndex tile = TileXY(x, 12);
		MakeRailNormal(tile, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		SB(_m[tile].m5, 6, 2, to_underlying(RailTileType::Signals));
		SetPresentSignals(tile, SignalAlongTrackdir(TRACKDIR_X_NE));
		SetSignalType(tile, TRACK_X, type);
	}
	void UpperSiding(bool exit_only)
	{
		MakeRailNormal(TileXY(8,10), _current_company, TRACK_BIT_X | TRACK_BIT_RIGHT | TRACK_BIT_UPPER, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(8,9), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(8,8), _current_company, TRACK_BIT_LOWER, RAILTYPE_RAIL);
		for (uint x = 9; x < 20; x++) MakeRailNormal(TileXY(x,8), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,8), _current_company, TRACK_BIT_RIGHT, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,9), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,10), _current_company, TRACK_BIT_X | TRACK_BIT_LOWER | TRACK_BIT_LEFT, RAILTYPE_RAIL);
		if (!exit_only) Marker(10,8,AutoYieldRole::Siding);
		Marker(18,8,AutoYieldRole::Exit);
	}
	void UseExitOnly()
	{
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,12), TRACK_X), false);
		program->items.clear();
		MakeRailNormal(TileXY(10,12), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		InvalidateAutoYieldAreas();
		UpdateAutoYieldAreas();
	}
};

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - ordinary slot associates signals and only lane claims are hidden")
{
	REQUIRE(TraceRestrictSlot::GetNumItems() == 2);
	CHECK(Resource(AUTO_YIELD_DETECTION_REF) != nullptr);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X)) != nullptr);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->max_occupancy == 1);
	GameRandomSeedChecker random_checker;
	const auto seed_order = slow->current_order.GetType();
	Execute(slow, 3, 10);
	CHECK(Resource(AUTO_YIELD_DETECTION_REF)->occupants.empty());
	CHECK(slow->current_order.GetType() == seed_order);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - faster following members deny main line without penalties")
{
	Register(slow); Register(medium);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::Faster));
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	const auto result = Execute(slow, 10, 10);
	CHECK((result.flags & TRPRF_DENY) != 0);
	CHECK(result.penalty == 0);
	CHECK((Execute(medium, 10, 10).flags & TRPRF_DENY) == 0);
	CHECK(GetRailReservationTrackBits(TileXY(10,12)) == TRACK_BIT_NONE);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - unavailable or unreachable sidings preserve ordinary passage")
{
	Register(slow); Register(medium);
	SECTION("siding occupied") { Place(fast, 14, 12); fast->vehstatus.Set(VehState::Stopped); }
	SECTION("train too long") { slow->gcache.cached_total_length = 20 * TILE_SIZE; }
	SECTION("branch already passed") { Place(slow, 12, 10); }
	SECTION("siding disconnected") { MakeRailNormal(TileXY(12,12), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL); }
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	const auto result = Execute(slow, 10, 10);
	CHECK((result.flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - priority ratio boundary and inactive candidates")
{
	Register(slow); Register(medium);
	bool expected = false;
	SECTION("exact 25 percent") { medium->vcache.cached_max_speed = 125; expected = true; }
	SECTION("below threshold") { medium->vcache.cached_max_speed = 124; }
	SECTION("instant speed does not matter") { medium->cur_speed = 0; expected = true; }
	SECTION("manually stopped") { medium->vehstatus.Set(VehState::Stopped); }
	SECTION("crashed") { medium->vehstatus.Set(VehState::Crashed); }
	SECTION("opposite direction") { medium->direction = Direction::NE; }
	SECTION("not registered") { Resource(AUTO_YIELD_DETECTION_REF)->Vacate(medium); }
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster) == expected);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - multiple faster members release after each tail clears")
{
	Register(slow); Register(medium); Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK_FALSE(EvaluateAutoYield(fast,membership->index.base(),AutoYieldCondition::MustWait));
	Place(fast,30,10);
	Execute(fast,27,10,TRPISP_RELEASE_FRONT);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::MustWait));
	Execute(fast,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	Place(medium,30,10); Execute(medium,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - admission stays open with transactional lane claims")
{
	Register(slow); Register(medium);
	TraceRestrictSlotTemporaryState temporary;
	temporary.PushToChangeStack();
	Execute(slow,10,12,TRPISP_ACQUIRE_TEMP_STATE);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(slow->index));
	CHECK((Execute(fast,3,10,TRPISP_ACQUIRE).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK_FALSE(Resource(AUTO_YIELD_DETECTION_REF)->IsOccupant(fast->index));
	temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->occupants.empty());
	Execute(slow,10,12,TRPISP_ACQUIRE);
	CHECK((Execute(medium,10,12).flags & TRPRF_DENY) != 0);
	CHECK((Execute(medium,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - full siding can safely vacate for a slower arrival")
{
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,10,12,TRPISP_ACQUIRE);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) != 0);
	Place(medium,30,10); Execute(medium,27,10,TRPISP_RELEASE_BACK);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - manual cancellation disables all conditions and frees the lane immediately")
{
	Register(slow); Register(medium);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X));
	REQUIRE(lane->IsOccupant(slow->index));
	GameRandomSeedChecker random_checker;
	SECTION("current registry") {}
	SECTION("configuration edit pending rebuild") { InvalidateAutoYieldAreas(); }
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute, membership->index, slow->index).Succeeded());
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK(lane->occupants.empty());
	UpdateAutoYieldAreas();
	for (uint8_t c = 0; c <= to_underlying(AutoYieldCondition::MayLeave); c++) {
		CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), static_cast<AutoYieldCondition>(c)));
		CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), static_cast<AutoYieldCondition>(c), true));
	}
	for (const auto &[x, y] : {std::pair{10,10}, {10,12}, {18,12}}) {
		CHECK((Execute(slow, x, y, TRPISP_ACQUIRE).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
	}
	CHECK(lane->occupants.empty());
	CHECK(membership->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(GetRailReservationTrackBits(TileXY(10,12)) == TRACK_BIT_NONE);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - slot zero and unconfigured role defaults are valid")
{
	REQUIRE(membership->index.base() == 0);
	for (uint8_t role = 0; role <= to_underlying(AutoYieldRole::Exit); role++) {
		TraceRestrictInstructionItem storage{};
		TraceRestrictInstructionItemRef item(storage);
		SetTraceRestrictTypeAndNormalise(item, TRIT_AUTO_YIELD_AREA, role);
		CHECK(item.GetValueAsSlot() == INVALID_TRACE_RESTRICT_SLOT_ID);
		CHECK(item.GetAuxField() == role);
		CHECK(GetTraceRestrictTypeProperties(item).value_type == TRVT_SLOT_INDEX);
	}
	Register(slow); Register(medium);
	CHECK(EvaluateAutoYield(slow, 0, AutoYieldCondition::Faster));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - reservation checks do not register outside trains and respect slot capacity")
{
	membership->max_occupancy = 1;
	TraceRestrictSlotTemporaryState temporary;
	temporary.PushToChangeStack();
	Execute(slow,3,10,TRPISP_ACQUIRE_TEMP_STATE);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
	CHECK(membership->occupants.empty());
	temporary.PushToChangeStack();
	Execute(slow,3,10,TRPISP_ACQUIRE_TEMP_STATE);
	temporary.PopFromChangeStackApplyTemporaryChanges(slow);
	CHECK(membership->occupants.empty());
	Execute(slow,3,10,TRPISP_ACQUIRE);
	Execute(slow,3,10,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK(membership->occupants.empty());
	Register(slow);
	CHECK((Execute(medium,3,10,TRPISP_ACQUIRE).flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK_FALSE(membership->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - committing a transactional release clears its lane while rollback preserves it")
{
	Register(slow);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X));
	TraceRestrictSlotTemporaryState temporary;
	temporary.PushToChangeStack();
	membership->VacateUsingTemporaryState(slow->index, &temporary);
	temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(lane->IsOccupant(slow->index));
	temporary.PushToChangeStack();
	membership->VacateUsingTemporaryState(slow->index, &temporary);
	temporary.PopFromChangeStackApplyTemporaryChanges(slow);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK_FALSE(lane->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - global switch disables conditions and claims while tail cleanup remains available")
{
	Register(slow); Register(medium);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	_settings_game.vehicle.automatic_yield = false;
	AutoYieldSettingsChanged();
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->occupants.empty());
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::Faster));
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::Faster, true));
	CHECK((Execute(slow,10,10).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
	Register(fast);
	CHECK_FALSE(membership->IsOccupant(fast->index));
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	_settings_game.vehicle.automatic_yield = true;
	AutoYieldSettingsChanged();
	CHECK(membership->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - deleting the user slot clears every reference before its ID is reused")
{
	Register(slow); Register(medium);
	const auto id = membership->index;
	TraceRestrictRemoveSlotID(id);
	delete membership;
	membership = nullptr;
	for (TraceRestrictProgram *program : TraceRestrictProgram::Iterate()) {
		CHECK(program->Validate().Succeeded());
		for (auto item : program->IterateInstructions()) CHECK(item.Instruction().GetValueAsSlot() == INVALID_TRACE_RESTRICT_SLOT_ID);
	}
	UpdateAutoYieldAreas();
	CHECK(TraceRestrictSlot::GetNumItems() == 0);
	CHECK_FALSE(EvaluateAutoYield(slow, id.base(), AutoYieldCondition::Faster));
	membership = TraceRestrictSlot::Create(_current_company);
	CHECK(membership->index == id);
	CHECK_FALSE(EvaluateAutoYield(slow, id.base(), AutoYieldCondition::MayLeave, true));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - version one conversion keeps source membership and lane claims")
{
	Register(slow); Register(medium);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	TraceRestrictSlot *lane = Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X));
	const auto source_id = membership->index;
	/* Simulate v1's numeric area, deliberately different from the slot ID. */
	for (TraceRestrictProgram *program : TraceRestrictProgram::Iterate()) {
		for (auto iter : program->IterateInstructionsMutable()) iter.InstructionRef().SetValue(42);
	}
	membership->automatic_yield_area = 42;
	lane->automatic_yield_area = 42;
	REQUIRE(MigrateAutoYieldAreasToSlots());
	UpdateAutoYieldAreas();
	CHECK(membership->index == source_id);
	CHECK(membership->automatic_yield_area == 0);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK(lane->IsOccupant(slow->index));
	CHECK(lane->automatic_yield_area == source_id.base() + 1);
	for (const TraceRestrictProgram *program : TraceRestrictProgram::Iterate()) {
		for (auto iter : program->IterateInstructions()) CHECK(iter.Instruction().GetValueAsSlot() == source_id);
	}
	CHECK(EvaluateAutoYield(slow, source_id.base(), AutoYieldCondition::MustWait));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - cancelled membership cannot trigger a negated program condition")
{
	Register(slow); Register(medium);
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute, membership->index, slow->index).Succeeded());
	TraceRestrictProgram *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,10), TRACK_X), false);
	TraceRestrictInstructionItem condition{};
	condition.SetType(TRIT_COND_AUTO_YIELD_FREE);
	condition.SetValue(membership->index);
	condition.SetCondOp(TRCO_ISNOT);
	TraceRestrictInstructionItem deny{};
	deny.SetType(TRIT_PF_DENY);
	TraceRestrictInstructionItem end{};
	end.SetType(TRIT_COND_ENDIF);
	program->items.insert(program->items.begin(), {condition.AsProgramItem(), deny.AsProgramItem(), end.AsProgramItem()});
	REQUIRE(program->Validate().Succeeded());
	CHECK((Execute(slow,10,10).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - snapshot and registry rebuild retain membership and opt out")
{
	Register(slow); Register(medium); Register(fast);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute, membership->index, fast->index).Succeeded());
	AutoRestoreBackup format(_savegame_format, std::string{"none"});
	AutoRestoreBackup cursor(_cursor.sprites, std::vector<CursorSprite>{{0, 0, 0, 0}});
	auto snapshot = std::make_shared<CoupleSnapshotCapture>();
	REQUIRE(SaveWithFilter(snapshot, false, SMF_NET_SERVER) == SaveLoadResult::Ok);
	for (const std::string &name : std::array<std::string, 5>{"pxp_automatic_yield", "vehicle.automatic_yield", "automatic_yield_area", "automatic_yield_signal", membership->name}) {
		CHECK(std::search(snapshot->bytes.begin(), snapshot->bytes.end(), name.begin(), name.end()) != snapshot->bytes.end());
	}
	const auto count = TraceRestrictSlot::GetNumItems();
	ResetAutoYieldAreas();
	UpdateAutoYieldAreas();
	CHECK(TraceRestrictSlot::GetNumItems() == count);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK_FALSE(membership->IsOccupant(fast->index));
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(slow->index));
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::MustWait));
	CHECK_FALSE(EvaluateAutoYield(fast, membership->index.base(), AutoYieldCondition::MayLeave));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - tail cleanup survives participant overflow and incomplete configuration")
{
	Register(slow); Register(medium); Register(fast);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	SECTION("participant overflow") {
		membership->max_occupancy = 32;
		for (uint i = 0; i < 14; i++) REQUIRE(membership->Occupy(MakeActor(2,10,100)));
		REQUIRE(membership->occupants.size() == 17);
	}
	SECTION("missing main marker") {
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,10), TRACK_X), false);
		program->items.clear();
		InvalidateAutoYieldAreas();
		UpdateAutoYieldAreas();
	}
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::MustWait));
	Execute(slow,27,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->IsOccupant(slow->index));
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	std::vector<TraceRestrictSlotID> held;
	TraceRestrictGetVehicleSlots(slow->index, held);
	CHECK(held.empty());
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - an exit alone identifies a siding and claims only after PBS reservation")
{
	UseExitOnly();
	Register(slow); Register(medium);
	const auto exit_ref = MakeTraceRestrictRefId(TileXY(18,12),TRACK_X);
	TraceRestrictSlot *lane = Resource(exit_ref);
	REQUIRE(lane != nullptr);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X)) == nullptr);
	CHECK(TraceRestrictSlot::GetNumItems() == 2);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	CHECK((Execute(slow,18,12,TRPISP_PBS_RES_END_ACQ_DRY).flags & TRPRF_PBS_RES_END_WAIT) == 0);
	CHECK(lane->occupants.empty());
	Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK(lane->IsOccupant(slow->index));
	const auto competing = Execute(medium,18,12,TRPISP_PBS_RES_END_ACQ_DRY);
	CHECK((competing.flags & TRPRF_PBS_RES_END_WAIT) != 0);
	CHECK((competing.flags & TRPRF_DENY) != 0);
	CHECK_FALSE(lane->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(GetRailReservationTrackBits(TileXY(18,12)) == TRACK_BIT_NONE);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - exit only temporary claims roll back and manual cancellation still releases")
{
	UseExitOnly();
	Register(slow); Register(medium);
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X));
	TraceRestrictSlotTemporaryState temporary;
	temporary.PushToChangeStack();
	Execute(slow,18,12,TRPISP_ACQUIRE_TEMP_STATE);
	CHECK(lane->IsOccupant(slow->index));
	temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
	CHECK(lane->occupants.empty());
	Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
	REQUIRE(lane->IsOccupant(slow->index));
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute, membership->index, slow->index).Succeeded());
	CHECK(lane->occupants.empty());
	CHECK((Execute(slow,18,12).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - exit only never treats a main line or a downstream merge as a free siding")
{
	UseExitOnly();
	Register(slow); Register(medium);
	SECTION("train longer than exclusive branch") { slow->gcache.cached_total_length = 20 * TILE_SIZE; }
	SECTION("exit on main line before merge") {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
		Marker(16,10,AutoYieldRole::Exit);
	}
	SECTION("exit after merge") {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
		Marker(24,10,AutoYieldRole::Exit);
	}
	SECTION("unreachable branch") { MakeRailNormal(TileXY(12,12),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL); }
	SECTION("occupied branch") { Place(fast,14,12); fast->vehstatus.Set(VehState::Stopped); }
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - one siding prioritises the slowest reachable contender")
{
	Register(slow); Register(medium); Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::NeedsSiding));
	CHECK_FALSE(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::NeedsSiding));
	CHECK((Execute(medium, 10, 10).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
	SECTION("slow train already passed the branch") {
		Place(slow, 12, 10);
		CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::NeedsSiding));
	}
	SECTION("slow train cannot fit") {
		slow->gcache.cached_total_length = 20 * TILE_SIZE;
		CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::NeedsSiding));
	}
	SECTION("slow train ahead is no longer registered") {
		membership->Vacate(slow);
		CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::NeedsSiding));
	}
	CHECK(random_checker.Check());
	CHECK(GetRailReservationTrackBits(TileXY(10,12)) == TRACK_BIT_NONE);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - parallel curves on one tile do not hide a siding")
{
	UseExitOnly();
	MakeRailNormal(TileXY(8,12), _current_company, TRACK_BIT_LEFT | TRACK_BIT_RIGHT, RAILTYPE_RAIL);
	Register(slow); Register(medium);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	CHECK(random_checker.Check());
	CHECK(GetRailReservationTrackBits(TileXY(18,12)) == TRACK_BIT_NONE);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a medium train ahead leaves the only siding for a slower train behind")
{
	Place(medium,7,10); Place(slow,5,10); Place(fast,4,10);
	Register(slow); Register(medium); Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK_FALSE(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::NeedsSiding));
	CHECK((Execute(medium,10,10).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
	Place(medium,12,10);
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::NeedsSiding));
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - only the approach marker supplies the speed and distance parameters")
{
	Marker(10,10,AutoYieldRole::Main, membership->index.base(), (1000u << 16) | 1u);
	Marker(18,12,AutoYieldRole::Exit, membership->index.base(), (1000u << 16) | 1u);
	Register(slow); Register(medium);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	Marker(3,10,AutoYieldRole::Approach, membership->index.base(), (1000u << 16) | 100u);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - front crossing registers a PBS train with a pre-existing route")
{
	const auto *approach = GetExistingTraceRestrictProgram(TileXY(3,10),TRACK_X);
	REQUIRE(approach != nullptr);
	CHECK((approach->actions_used_flags & TRPAUF_SLOT_RELEASE_FRONT) != 0);
	CHECK((GetExistingTraceRestrictProgram(TileXY(10,10),TRACK_X)->actions_used_flags & TRPAUF_SLOT_RELEASE_FRONT) == 0);
	GameRandomSeedChecker random_checker;
	Execute(slow,3,10); // A route query must never register a train.
	CHECK_FALSE(membership->IsOccupant(slow->index));
	Execute(slow,3,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->IsOccupant(slow->index));
	Execute(slow,3,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->occupants.size() == 1);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->occupants.empty());
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - physical front crossing respects capacity direction and the switch")
{
	SECTION("ordinary capacity") {
		membership->max_occupancy = 1;
		Register(medium);
		Execute(slow,3,10,TRPISP_RELEASE_FRONT);
		CHECK_FALSE(membership->IsOccupant(slow->index));
		CHECK(membership->occupants.size() == 1);
	}
	SECTION("back of signal") {
		TraceRestrictProgramInput input(TileXY(3,10),TRACKDIR_X_NE,nullptr,nullptr);
		input.permitted_slot_operations = TRPISP_RELEASE_FRONT;
		TraceRestrictProgramResult out;
		GetExistingTraceRestrictProgram(input.tile,TRACK_X)->Execute(slow,input,out);
		CHECK_FALSE(membership->IsOccupant(slow->index));
	}
	SECTION("disabled") {
		_settings_game.vehicle.automatic_yield = false;
		Execute(slow,3,10,TRPISP_RELEASE_FRONT);
		CHECK_FALSE(membership->IsOccupant(slow->index));
	}
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - detection registers physical passage without reserving membership")
{
	Register(slow);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	CHECK((Execute(fast,3,10,TRPISP_ACQUIRE).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK_FALSE(membership->IsOccupant(fast->index));
	Execute(fast,3,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->IsOccupant(fast->index));
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - legacy distance bits never limit speed detection")
{
	const uint32_t old_distance = GENERATE(0u,1u,100u,4096u,65535u);
	auto *legacy=Marker(3,10,AutoYieldRole::Approach,membership->index.base(),(25u << 16) | old_distance);
	MigrateLegacyAutoYieldSpeedCaps(*legacy);
	InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
	CHECK(legacy->items[1].base()==TEST_YIELD_CONFIG);
	Register(slow);Register(medium);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
}

class AutomaticYieldWideFixture : public AutomaticYieldFixture {
protected:
	AutomaticYieldWideFixture() : AutomaticYieldFixture(2048) {}
};

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield - a distant registered follower is not discarded by distance or probe limits")
{
	GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,10),TRACK_X),false)->items.clear();
	for(uint x=41;x<=1801;x++) MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Marker(1800,10,AutoYieldRole::Main);
	Place(slow,1790,10);Place(fast,2,10);
	MakeRailNormal(TileXY(900,50),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Place(medium,900,50);
	Register(slow);Register(fast);Register(medium);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	AutoRestoreBackup tick_backup(_state_ticks, StateTicks{64});
	AutoYieldGameTick();
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(fast->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - clearing all source members releases hidden lane claims")
{
	Register(slow);Register(medium);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	auto *lane=Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X));
	REQUIRE(lane->IsOccupant(slow->index));
	GameRandomSeedChecker random_checker;
	membership->Clear();
	CHECK(membership->occupants.empty());
	CHECK(lane->occupants.empty());
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - crossing occupancy never proves an empty siding")
{
	Register(slow);Register(medium);
	MakeRailNormal(TileXY(14,12),_current_company,TRACK_BIT_X | TRACK_BIT_Y,RAILTYPE_RAIL);
	SECTION("physical crossing occupant") {
		fast->track=TRACK_BIT_Y;Place(fast,14,12);
		fast->vehstatus.Set(VehState::Stopped);
	}
	SECTION("crossing reservation") {
		REQUIRE(TryReserveRailTrack(TileXY(14,12),TRACK_Y));
	}
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - station platforms are usable but occupied platform tiles remain protected")
{
	UseExitOnly();
	const uint platform_length = GENERATE(1u, 5u);
	Station *station = Station::Create(TileXY(11,12));
	station->owner = _current_company;
	station->facilities = {StationFacility::Train};
	station->train_station = TileArea(station->xy, platform_length, 1);
	station->rect.BeforeAddTile(station->xy, StationRect::ADD_FORCE);
	station->rect.BeforeAddTile(TileXY(10 + platform_length,12), StationRect::ADD_FORCE);
	for (uint x = 11; x < 11 + platform_length; x++) MakeRailStation(TileXY(x,12), _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
	Register(slow); Register(medium);
	GameRandomSeedChecker random_checker;
	SECTION("empty platform") {
		CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
		Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
		CHECK(Resource(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X))->IsOccupant(slow->index));
		CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::MustWait));
	}
	SECTION("another train on a skipped middle platform tile") {
		Place(fast,10 + (platform_length + 1) / 2,12);
		CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	SECTION("unattributed reservation on a skipped middle platform tile") {
		SetRailStationReservation(TileXY(10 + (platform_length + 1) / 2,12),true);
		CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	CHECK(random_checker.Check());
	_station_pool.CleanPool();
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - ordinary bridges do not read simulated signal fields")
{
	Register(slow); Register(medium);
	MakeRailBridgeRamp(TileXY(12,12), _current_company, 0, DiagDirection::SW, RAILTYPE_RAIL, false);
	MakeRailBridgeRamp(TileXY(16,12), _current_company, 0, DiagDirection::NE, RAILTYPE_RAIL, false);
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - reverse one way signals cannot prove an available siding")
{
	UseExitOnly();
	Register(slow); Register(medium);
	const uint x = GENERATE(17u, 19u);
	const SignalType type = GENERATE(SignalType::Block, SignalType::PathOneWay);
	BackSignal(x, type);
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - reverse passable PBS preserves existing back face restrictions")
{
	UseExitOnly();
	Register(slow); Register(medium);
	const uint x = GENERATE(17u, 19u);
	BackSignal(x, SignalType::Path);
	bool denied = false;
	SECTION("plain reverse PBS") {}
	SECTION("reverse PBS denies its facing direction") {
		denied = true;
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(x,12), TRACK_X), true);
		TraceRestrictInstructionItem deny{}; deny.SetType(TRIT_PF_DENY);
		program->items = {deny.AsProgramItem()};
		REQUIRE(program->Validate().Succeeded());
		InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	}
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding) == !denied);
	CHECK(((Execute(slow,10,10).flags & TRPRF_DENY) != 0) == !denied);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - exit markers require one signal face in the departure direction")
{
	UseExitOnly();
	Register(slow); Register(medium);
	SECTION("exit faces the wrong way") { SetPresentSignals(TileXY(18,12), SignalAlongTrackdir(TRACKDIR_X_NE)); }
	SECTION("exit has both faces") { SetPresentSignals(TileXY(18,12), SignalAlongTrackdir(TRACKDIR_X_NE) | SignalAlongTrackdir(TRACKDIR_X_SW)); }
	InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	CHECK((Execute(slow,18,12).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - an empty alternative siding avoids unnecessary evacuation")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	UpperSiding(exit_only);
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12);
	Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags & TRPRF_WAIT_AT_PBS) != 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a train past the fork must not evict a siding occupant")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	Place(slow,9,10);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - evacuation cannot cut ahead of a faster train already passing")
{
	const uint fast_x = GENERATE(9u, 12u);
	Register(slow); Register(medium); Register(fast);
	Train *rear = MakeActor(4,10,220); Register(rear);
	Place(fast,fast_x,10);
	Place(medium,14,12); Execute(medium,10,12,TRPISP_ACQUIRE);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags & TRPRF_WAIT_AT_PBS) != 0);
	Execute(fast,27,10,TRPISP_RELEASE_BACK);
	Place(fast,30,10);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - eviction requires an eligible unblocked incoming route")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	SECTION("another train blocks the incoming branch") {
		Train *blocker = MakeActor(12,12,30); blocker->vehstatus.Set(VehState::Stopped);
	}
	SECTION("ordinary restriction denies the incoming branch") {
		BackSignal(11,SignalType::Path);
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(11,12),TRACK_X),true);
		TraceRestrictInstructionItem deny{}; deny.SetType(TRIT_PF_DENY);
		program->items = {deny.AsProgramItem()};
		REQUIRE(program->Validate().Succeeded());
		InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	}
	SECTION("incoming train cannot fit") { slow->gcache.cached_total_length = 20 * TILE_SIZE; }
	SECTION("only the departing train can use the rail type after the merge") {
		medium->railtypes.Set(RAILTYPE_ELECTRIC);
		medium->compatible_railtypes.Set(RAILTYPE_ELECTRIC);
		MakeRailNormal(TileXY(23,10),_current_company,TRACK_BIT_X,RAILTYPE_ELECTRIC);
	}
	SECTION("outgoing route is occupied") {
		Train *blocker = MakeActor(22,10,30); blocker->vehstatus.Set(VehState::Stopped);
	}
	GameRandomSeedChecker random_checker;
	const auto members = membership->occupants;
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X));
	REQUIRE(lane != nullptr);
	const auto claims = lane->occupants;
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK(membership->occupants == members);
	CHECK(lane->occupants == claims);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a departure proposal never releases live reservations or lane claims")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	REQUIRE(TryReserveRailTrack(TileXY(14,12),TRACK_X));
	REQUIRE(GetTrainForReservation(TileXY(14,12),TRACK_X) == medium);
	const auto members = membership->occupants;
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X));
	REQUIRE(lane != nullptr);
	const auto claims = lane->occupants;
	GameRandomSeedChecker random_checker;
	for (uint i = 0; i < 8; i++) {
		CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
		CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) != 0);
		CHECK(GetReservedTrackbits(TileXY(14,12)) == TRACK_BIT_X);
		CHECK(membership->occupants == members);
		CHECK(lane->occupants == claims);
	}
	CHECK(random_checker.Check());
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - only the selected incoming train waits for the departure proposal")
{
	Register(slow); Register(medium); Register(fast);
	Place(slow,9,10);
	Train *incoming = MakeActor(7,10,80); Register(incoming);
	Place(medium,14,12); Execute(medium,10,12,TRPISP_ACQUIRE);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(incoming,10,10).flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - physical detection remains usable when downstream roles are incomplete")
{
	UseExitOnly();
	SECTION("only exit becomes double faced") {
		SetPresentSignals(TileXY(18,12),SignalAlongTrackdir(TRACKDIR_X_NE) | SignalAlongTrackdir(TRACKDIR_X_SW));
	}
	SECTION("main marker is removed") {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,10),TRACK_X),false)->items.clear();
	}
	SECTION("exit marker is removed") {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
	}
	InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	GameRandomSeedChecker random_checker;
	Register(slow); Register(medium);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,18,12).flags & TRPRF_WAIT_AT_PBS) == 0);
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - physical registration does not depend on a clean area cache")
{
	InvalidateAutoYieldAreas();
	Execute(slow,3,10,TRPISP_ACQUIRE_TEMP_STATE);
	CHECK(membership->occupants.empty());
	Register(slow);
	CHECK(membership->IsOccupant(slow->index));
	UpdateAutoYieldAreas();
	CHECK(membership->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - multiple approach markers keep their shared area active")
{
	UseExitOnly();
	Marker(5,10,AutoYieldRole::Approach);
	Register(slow); Register(medium);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	const auto members = membership->occupants;
	Execute(slow,5,10,TRPISP_RELEASE_FRONT);
	Execute(medium,5,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->occupants == members);
	Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK((Execute(medium,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK((Execute(medium,5,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK((Execute(fast,5,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - multiple approach thresholds use the strictest value independent of tile order")
{
	const bool strict_first = GENERATE(false, true);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),(strict_first ? 50u : 5u) << 16);
	Marker(5,10,AutoYieldRole::Approach,membership->index.base(),(strict_first ? 5u : 50u) << 16);
	medium->vcache.cached_max_speed = 140;
	Register(slow); Register(medium);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - repeated approach crossings never consume another capacity slot")
{
	Marker(5,10,AutoYieldRole::Approach);
	membership->max_occupancy = 1;
	Register(slow);
	Execute(slow,5,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->occupants.size() == 1);
	CHECK((Execute(medium,5,10).flags & TRPRF_WAIT_AT_PBS) != 0);
	Execute(medium,5,10,TRPISP_RELEASE_FRONT);
	CHECK_FALSE(membership->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

class AutomaticYieldPlatformFixture : public AutomaticYieldFixture {
protected:
	Station *platform_station = nullptr;
	~AutomaticYieldPlatformFixture() { _station_pool.CleanPool(); }
	void SetupPlatform(bool exit_only)
	{
		if (exit_only) UseExitOnly();
		UpperSiding(exit_only);
		platform_station = Station::Create(TileXY(11,12));
		platform_station->owner = _current_company;
		platform_station->facilities = {StationFacility::Train};
		platform_station->train_station = TileArea(platform_station->xy, 5, 1);
		platform_station->rect.BeforeAddTile(platform_station->xy, StationRect::ADD_FORCE);
		platform_station->rect.BeforeAddTile(TileXY(15,12), StationRect::ADD_FORCE);
		for (uint x = 11; x <= 15; x++) MakeRailStation(TileXY(x,12),_current_company,platform_station->index,Axis::X,0,RAILTYPE_RAIL);
		Marker(18,12,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
	}
	void PassStation(Train *v)
	{
		v->current_order.MakeGoToStation(platform_station->index);
		v->current_order.SetNonStopType(ONSF_NO_STOP_AT_ANY_STATION);
	}
};

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - passing trains prefer a usable ordinary siding")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	PassStation(slow); Register(slow); Register(medium);
	GameRandomSeedChecker random_checker;
	const auto platform = Execute(slow,exit_only ? 18 : 10,12);
	CHECK((platform.flags & TRPRF_DENY) != 0);
	CHECK((Execute(slow,exit_only ? 18 : 10,8).flags & TRPRF_DENY) == 0);
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - platform fallback does not wait for an unusable ordinary siding")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	PassStation(slow); Register(slow); Register(medium);
	SECTION("ordinary siding occupied") { Place(fast,14,8); fast->vehstatus.Set(VehState::Stopped); }
	SECTION("ordinary siding claimed") {
		auto *lane = Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,8),TRACK_X));
		REQUIRE(lane != nullptr); REQUIRE(lane->Occupy(fast));
	}
	SECTION("ordinary siding blocked by reverse one way signal") {
		Marker(14,8,AutoYieldRole::Approach,INVALID_TRACE_RESTRICT_SLOT_ID.base());
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(14,8),TRACK_X),false)->items.clear();
		SetPresentSignals(TileXY(14,8),SignalAlongTrackdir(TRACKDIR_X_NE));
		SetSignalType(TileXY(14,8),TRACK_X,SignalType::PathOneWay);
		InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	}
	CHECK((Execute(slow,exit_only ? 18 : 10,12).flags & TRPRF_DENY) == 0);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - scheduled stops and coupling destinations remain eligible")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	Register(slow); Register(medium);
	SECTION("scheduled station stop") { slow->current_order.MakeGoToStation(platform_station->index); }
	SECTION("intermediate stop is allowed") { slow->current_order.MakeGoToStation(StationID{123}); slow->current_order.SetNonStopType(ONSF_STOP_EVERYWHERE); }
	SECTION("coupling target") { slow->current_order.MakeGoToCouple(); }
	SECTION("waiting for coupling") { slow->current_order.MakeWaitCouple(); }
	CHECK((Execute(slow,exit_only ? 18 : 10,12).flags & TRPRF_DENY) == 0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - a committed platform route is not rejected after ordinary capacity frees")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	PassStation(slow); Register(slow); Register(medium);
	Place(fast,14,8); fast->vehstatus.Set(VehState::Stopped);
	Execute(slow,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	REQUIRE(Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X))->IsOccupant(slow->index));
	Place(fast,30,10);
	CHECK((Execute(slow,18,12).flags & TRPRF_DENY) == 0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - station work cannot freeze ordinary traffic or incoming coupling")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	Register(slow); Register(medium);
	Place(medium,14,12);
	Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	medium->last_station_visited = platform_station->index;
	SECTION("loading or full load") { medium->current_order.MakeLoading(true); }
	SECTION("waiting coupling") { medium->current_order.MakeWaitCouple(); }
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	Register(fast);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK(membership->IsOccupant(medium->index));
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X))->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - an unclaimed loading train is identified on its actual platform")
{
	SetupPlatform(true);
	Register(slow); Register(medium);
	Place(medium,14,12);
	medium->last_station_visited = platform_station->index;
	medium->current_order.MakeLoading(true);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - marking one platform does not change unmarked tracks at the same station")
{
	SetupPlatform(true);
	platform_station->train_station = TileArea(TileXY(11,8),5,5);
	platform_station->rect.BeforeAddTile(TileXY(14,8),StationRect::ADD_FORCE);
	MakeRailStation(TileXY(14,8),_current_company,platform_station->index,Axis::X,0,RAILTYPE_RAIL);
	Register(slow); Register(medium);
	Place(medium,14,8);
	Execute(medium,18,8,TRPISP_ACQUIRE);
	medium->last_station_visited = platform_station->index;
	medium->current_order.MakeLoading(true);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - authoritative commands preserve slot IDs and normalise lane flags")
{
	SetupPlatform(true);
	const TileIndex tile = TileXY(18,12);
	const auto ref = MakeTraceRestrictRefId(tile,TRACK_X);
	const uint32_t config = TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM;
	REQUIRE(CmdProgramSignalTraceRestrict({},tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,TEST_YIELD_CONFIG,"").Succeeded());
	CHECK(GetTraceRestrictProgram(ref,false)->items[1].base() == config);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,TEST_YIELD_CONFIG,"").Succeeded());
	CHECK(GetTraceRestrictProgram(ref,false)->items[1].base() == TEST_YIELD_CONFIG);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,config,"").Succeeded());
	CHECK(GetTraceRestrictProgram(ref,false)->items[1].base() == config);
	CHECK(TraceRestrictInstructionItem(GetTraceRestrictProgram(ref,false)->items[0].base()).GetValueAsSlot() == membership->index);
	TraceRestrictInstructionItem approach(GetTraceRestrictProgram(ref,false)->items[0].base());
	approach.SetAuxField(to_underlying(AutoYieldRole::Approach));
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_ITEM,0,approach.base(),"").Succeeded());
	CHECK((GetTraceRestrictProgram(ref,false)->items[1].base() & AUTO_YIELD_PLATFORM) == 0);
	CHECK(CmdProgramSignalTraceRestrict({},tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,config,"").Failed());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - a long train with its front beyond the platform can finish station work")
{
	SetupPlatform(true);
	Register(slow); Register(medium);
	Place(medium,19,12);
	Train *rear = MakeTrain(); medium->SetNext(rear);
	rear->owner = _current_company; rear->track = TRACK_BIT_X; rear->direction = Direction::SW;
	Place(rear,14,12);
	medium->last_station_visited = platform_station->index;
	medium->current_order.MakeLoading(true);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - program copying retains the platform option")
{
	SetupPlatform(true);
	REQUIRE(CmdProgramSignalTraceRestrictMgmt(DoCommandFlag::Execute,TileXY(18,8),TRACK_X,TRMDCT_PROG_COPY,TileXY(18,12),TRACK_X).Succeeded());
	auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,8),TRACK_X),false);
	REQUIRE(program != nullptr);
	CHECK((program->items[1].base() & AUTO_YIELD_PLATFORM) != 0);
	CHECK(program->Validate().Succeeded());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - complex alternatives cannot consume the budget needed for safe waiting")
{
	SetupPlatform(true);
	PassStation(slow); Register(slow); Register(medium);
	Place(fast,14,8); fast->vehstatus.Set(VehState::Stopped);
	for (uint y = 20; y <= 56; y += 4) {
		for (uint x = 1; x <= 60; x++) {
			MakeRailNormal(TileXY(x,y),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
			MakeRailNormal(TileXY(x,y-2),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		}
		MakeRailNormal(TileXY(1,y),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(1,y-1),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(1,y-2),_current_company,TRACK_BIT_LOWER,RAILTYPE_RAIL);
		Marker(60,y,AutoYieldRole::Exit);
	}
	const auto result = Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK((result.flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X))->IsOccupant(slow->index));
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - unknown legacy platform geometry cannot force station work to vacate")
{
	SetupPlatform(false);
	GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
	Marker(24,10,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
	Register(slow); Register(medium); Register(fast);
	Train *blocker = MakeActor(14,8,30); blocker->vehstatus.Set(VehState::Stopped);
	Place(medium,14,12); Execute(medium,10,12,TRPISP_ACQUIRE);
	medium->last_station_visited = platform_station->index;
	medium->current_order.MakeLoading(true);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - Wangjia detector does not close behind an occupied siding")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(medium);
	Place(slow,14,12);
	Execute(slow,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X));
	REQUIRE(lane->IsOccupant(slow->index));
	Place(fast,2,10);
	GameRandomSeedChecker random_checker;
	SECTION("capacity available: a new fast train may enter") {
		CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
		CHECK((Execute(fast,3,10,TRPISP_ACQUIRE_TEMP_STATE).flags & TRPRF_WAIT_AT_PBS) == 0);
		CHECK_FALSE(membership->IsOccupant(fast->index));
		Execute(fast,3,10,TRPISP_RELEASE_FRONT);
		CHECK(membership->IsOccupant(fast->index));
	}
	SECTION("capacity available: admission does not depend on speed") {
		fast->vcache.cached_max_speed = 80;
		CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	}
	SECTION("full source still stops new trains") {
		membership->max_occupancy = 2;
		CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) != 0);
		Execute(fast,3,10,TRPISP_RELEASE_FRONT);
		CHECK_FALSE(membership->IsOccupant(fast->index));
		CHECK((Execute(medium,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	}
	CHECK(lane->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - a needed fallback ignores numeric penalties without bypassing safety")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	PassStation(slow); Register(slow); Register(medium);
	Place(fast,14,8); fast->vehstatus.Set(VehState::Stopped);
	const uint x = exit_only ? 18 : 10;
	auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(x,12),TRACK_X),false);
	TraceRestrictInstructionItem penalty{0}; penalty.SetType(TRIT_PF_PENALTY); penalty.SetValue(60000);
	program->items.insert(program->items.begin(),penalty.AsProgramItem());
	program->items.push_back(penalty.AsProgramItem());
	REQUIRE(program->Validate().Succeeded());
	GameRandomSeedChecker random_checker;
	SECTION("slow train must use the safe empty platform despite penalties on both sides of the marker") {
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
		const auto out = Execute(slow,x,12);
		CHECK(out.penalty == 0);
		CHECK((out.flags & TRPRF_DENY) == 0);
	}
	SECTION("unnecessary or unregistered diversions retain penalties") {
		medium->vcache.cached_max_speed = 100;
		CHECK(Execute(slow,x,12).penalty == 120000);
		membership->Vacate(slow);
		CHECK(Execute(slow,x,12).penalty == 120000);
	}
	SECTION("ordinary space retains ordinary preference") {
		Place(fast,30,10);
		const auto out = Execute(slow,x,12);
		CHECK((out.flags & TRPRF_DENY) != 0);
		CHECK(out.penalty == 120000);
	}
	SECTION("short platform cannot gain preference") {
		slow->gcache.cached_total_length = 40 * TILE_SIZE;
		CHECK(Execute(slow,x,12).penalty == 120000);
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	SECTION("occupied platform is protected") {
		Train *blocker = MakeActor(13,12,80); blocker->vehstatus.Set(VehState::Stopped);
		CHECK(Execute(slow,x,12).penalty == 120000);
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	SECTION("claimed platform is protected") {
		auto *lane = Resource(MakeTraceRestrictRefId(TileXY(x,12),TRACK_X));
		REQUIRE(lane->Occupy(fast));
		CHECK(Execute(slow,x,12).penalty == 120000);
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	SECTION("blocked departure cannot justify forcing the platform") {
		Train *blocker = MakeActor(25,10,80); blocker->vehstatus.Set(VehState::Stopped);
		CHECK(Execute(slow,x,12).penalty == 120000);
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	SECTION("mandatory station and coupling tasks keep their own rules") {
		slow->current_order.MakeGoToStation(platform_station->index);
		CHECK(Execute(slow,x,12).penalty == 120000);
		slow->current_order.MakeGoToCouple();
		CHECK(Execute(slow,x,12).penalty == 120000);
	}
	SECTION("global switch disables the cost override") {
		_settings_game.vehicle.automatic_yield = false;
		CHECK(Execute(slow,x,12).penalty == 120000);
	}
	SECTION("explicit deny remains authoritative") {
		TraceRestrictInstructionItem deny{0}; deny.SetType(TRIT_PF_DENY);
		program->items.push_back(deny.AsProgramItem());
		REQUIRE(program->Validate().Succeeded());
		CHECK((Execute(slow,x,12).flags & TRPRF_DENY) != 0);
	}
	SECTION("explicit PBS waiting is not cancelled") {
		TraceRestrictInstructionItem wait{0}; wait.SetType(TRIT_WAIT_AT_PBS);
		program->items.push_back(wait.AsProgramItem());
		REQUIRE(program->Validate().Succeeded());
		CHECK((Execute(slow,x,12).flags & TRPRF_WAIT_AT_PBS) != 0);
	}
	CHECK(random_checker.Check());
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield - Dezhou main admission and evacuation share their route proofs")
{
	const bool exit_only = GENERATE(false,true);
	for (const auto &[x,y] : {std::pair{10,10},{10,12},{18,12},{27,10}}) {
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(x,y),TRACK_X),false);
		REQUIRE(program != nullptr); program->items.clear(); REQUIRE(program->Validate().Succeeded());
	}
	for (uint x=1; x<=240; x++) MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	for (uint x=208; x<=220; x++) MakeRailNormal(TileXY(x,12),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(208,10),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(208,11),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(208,12),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(220,12),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(220,11),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(220,10),_current_company,TRACK_BIT_X|TRACK_BIT_LOWER,RAILTYPE_RAIL);
	Marker(3,10,AutoYieldRole::Approach);
	Marker(210,10,AutoYieldRole::Main);
	if (!exit_only) Marker(210,12,AutoYieldRole::Siding);
	Marker(218,12,AutoYieldRole::Exit);
	Marker(227,10,AutoYieldRole::Clear);
	Place(slow,30,10); Place(medium,214,12); Place(fast,1,10);
	Register(slow); Register(medium); Register(fast);
	Execute(medium,exit_only ? 218 : 210,12,TRPISP_ACQUIRE);
	REQUIRE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	GameRandomSeedChecker random_checker;
	const auto main=Execute(slow,210,10);
	CHECK((main.flags&TRPRF_WAIT_AT_PBS)!=0);
	CHECK((main.flags&TRPRF_PBS_RES_END_WAIT)!=0);
	CHECK_FALSE(IsWaitingPositionFreeTraceRestrictExecute(GetExistingTraceRestrictProgram(TileXY(210,10),TRACK_X),slow,TileXY(210,10),TRACKDIR_X_SW));
	CHECK((main.flags&TRPRF_DENY)==0); // Wait for the selected departure; do not force entry into an occupied siding.
	CHECK((Execute(medium,218,12).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a departing tail retains the incoming train's turn until clearance")
{
	const bool exit_only = GENERATE(false,true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	Train *rear = MakeTrain(); medium->SetNext(rear);
	rear->owner = _current_company; rear->track = TRACK_BIT_X; rear->direction = Direction::SW;
	Train *middle = MakeTrain(); medium->SetNext(middle); middle->SetNext(rear);
	middle->owner = _current_company; middle->track = TRACK_BIT_X; middle->direction = Direction::SW;
	Place(medium,30,10); Place(middle,27,10); Place(rear,25,10);
	medium->gcache.cached_total_length = 6 * TILE_SIZE;
	REQUIRE(membership->IsOccupant(medium->index));
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X))->IsOccupant(medium->index));
	Execute(medium,27,10,TRPISP_RELEASE_BACK);
	Place(middle,29,10); Place(rear,29,10);
	CHECK_FALSE(membership->IsOccupant(medium->index));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - route proofs distinguish own junction reservations from foreign ones")
{
	Register(slow); Register(medium); Register(fast);
	REQUIRE(TryReserveRailTrack(TileXY(8,10),TRACK_X));
	REQUIRE(TryReserveRailTrack(TileXY(9,10),TRACK_X));
	SECTION("own main reservation does not conceal the diverging siding") {
		REQUIRE(TryReserveRailTrack(TileXY(7,10),TRACK_X));
		REQUIRE(GetTrainForReservation(TileXY(8,10),TRACK_X) == slow);
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	}
	SECTION("another train's main reservation is still protected") {
		Place(medium,9,10);
		REQUIRE(GetTrainForReservation(TileXY(8,10),TRACK_X) == medium);
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	}
	SECTION("unattributed crossing reservation is still protected") {
		CHECK(GetTrainForReservation(TileXY(8,10),TRACK_X) == nullptr);
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	}
	CHECK(GetReservedTrackbits(TileXY(8,10)) == TRACK_BIT_X);
	CHECK(GetReservedTrackbits(TileXY(9,10)) == TRACK_BIT_X);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield - ordinary platform and mixed layouts share safe waiting and turnover")
{
	const bool exit_only = GENERATE(false,true);
	const uint layout = GENERATE(0u,1u,2u); // Ordinary only, platform only, mixed.
	SetupPlatform(exit_only); PassStation(slow);
	if (layout == 0) Marker(18,12,AutoYieldRole::Exit);
	if (layout == 1) {
		for (uint x=11; x<=15; x++) MakeRailStation(TileXY(x,8),_current_company,platform_station->index,Axis::X,0,RAILTYPE_RAIL);
		platform_station->train_station = TileArea(TileXY(11,8),5,5);
		platform_station->rect.BeforeAddTile(TileXY(11,8),StationRect::ADD_FORCE);
		Marker(18,8,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG|AUTO_YIELD_PLATFORM);
	}
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	GameRandomSeedChecker random_checker;
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags&TRPRF_WAIT_AT_PBS)!=0);
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	Train *blocker = MakeActor(14,8,30); blocker->vehstatus.Set(VehState::Stopped);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags&TRPRF_WAIT_AT_PBS)!=0);
	Place(medium,30,10); Execute(medium,27,10,TRPISP_RELEASE_BACK);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a proven opposing train does not keep a siding waiting")
{
	Register(slow); Register(fast);
	medium->vehstatus.Set(VehState::Stopped);
	GameRandomSeedChecker random_checker;
	SECTION("opposing faster train remains in the same source slot") {
		fast->direction = Direction::NE;
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
		CHECK((Execute(slow,18,12).flags&TRPRF_WAIT_AT_PBS)==0);
		fast->direction = Direction::SW;
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
		fast->direction = Direction::NE;
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	SECTION("a following fast train still has priority") {
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	SECTION("a faster train between Main and Clear is still passing") {
		Place(fast,15,10);
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	SECTION("a passing fast train remains protected until its tail clears") {
		Place(fast,30,10);
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
		Execute(fast,27,10,TRPISP_RELEASE_BACK);
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	CHECK(membership->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}


class AutomaticYieldMultipleMainFixture : public AutomaticYieldFixture {
protected:
	void ParallelBranch()
	{
		UpperSiding(true);
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,8),TRACK_X),false)->items.clear();
		MakeRailNormal(TileXY(18,8), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		Marker(10,8,AutoYieldRole::Main);
	}
	void IndependentApproach()
	{
		for (uint x=1;x<24;x++) MakeRailNormal(TileXY(x,16),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(24,16),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
		for (uint y=11;y<16;y++) MakeRailNormal(TileXY(24,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(24,10),_current_company,TRACK_BIT_X|TRACK_BIT_LOWER,RAILTYPE_RAIL);
		Marker(3,16,AutoYieldRole::Approach);
		Marker(10,16,AutoYieldRole::Main);
	}
};

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - an unrelated main does not disable the area")
{
	Marker(10,16,AutoYieldRole::Main);
	Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - different approach distances do not imply a follower")
{
	IndependentApproach();
	Place(fast,2,16);
	Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&(TRPRF_DENY|TRPRF_WAIT_AT_PBS))==0);
	// Once waiting in a shared siding, the merging fast train still has priority.
	Place(slow,14,12);Execute(slow,10,12,TRPISP_ACQUIRE);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Evacuate));
	Execute(fast,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - usable parallel main avoids a diversion but blocked tracks do not")
{
	ParallelBranch();Register(slow);Register(fast);
	bool parallel_free=false;
	SECTION("a free parallel track can carry normal traffic") { parallel_free=true; }
	SECTION("an occupant beyond the parallel main prevents bypass") { Place(medium,14,8);medium->vehstatus.Set(VehState::Stopped); }
	SECTION("an unknown PBS owner prevents bypass") { SetTrackReservation(TileXY(14,8),TRACK_BIT_X); }
	SECTION("a disconnected parallel track prevents bypass") { MakeRailNormal(TileXY(14,8),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL); }
	SECTION("ordinary path restrictions still apply") {
		auto *p=Marker(14,8,AutoYieldRole::Approach,INVALID_TRACE_RESTRICT_SLOT_ID.base());
		TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items={deny.AsProgramItem()};REQUIRE(p->Validate().Succeeded());
		InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
	}
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==!parallel_free);
	CHECK(((Execute(slow,10,10).flags&TRPRF_DENY)!=0)==!parallel_free);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - serial markers are not a parallel escape route")
{
	Marker(14,10,AutoYieldRole::Main);Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	Place(slow,12,10);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - extra mains cannot fabricate an evacuation")
{
	IndependentApproach();Place(fast,2,16);Register(slow);Register(fast);Register(medium);
	Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags&TRPRF_WAIT_AT_PBS)==0);
	// A real faster follower on the same main can still trigger a safe turnover.
	Place(fast,4,10);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags&TRPRF_WAIT_AT_PBS)!=0);
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - marker cap keeps rebuild and evaluation bounded")
{
	const uint mains=GENERATE(16u,17u);
	for(uint i=1;i<mains;i++)Marker(2+i,20,AutoYieldRole::Main);
	Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==(mains==16));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}


TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - a third free branch is checked after a blocked alternative")
{
	ParallelBranch();
	// A Main on the former siding supplies a third route, with a separate
	// exit-only lower siding so that all three roles remain valid.
	GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X),false)->items.clear();
	GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
	MakeRailNormal(TileXY(18,12),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Marker(10,12,AutoYieldRole::Main);
	// Keep an unreachable but valid siding exit: it must not affect the bypass proof.
	Marker(18,20,AutoYieldRole::Exit);
	Register(slow);Register(fast);
	Place(medium,14,8);medium->vehstatus.Set(VehState::Stopped);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&(TRPRF_DENY|TRPRF_WAIT_AT_PBS))==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - rebuilding preserves exclusive claims and manual opt out")
{
	ParallelBranch();Register(slow);Register(fast);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	const auto lane_id=Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->index;
	const auto slot_count=TraceRestrictSlot::GetNumItems();
	ResetAutoYieldAreas();UpdateAutoYieldAreas();
	CHECK(TraceRestrictSlot::GetNumItems()==slot_count);
	CHECK(TraceRestrictSlot::Get(lane_id)->IsOccupant(slow->index));
	CHECK((Execute(fast,10,12,TRPISP_ACQUIRE).flags&TRPRF_WAIT_AT_PBS)!=0);
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute,membership->index,slow->index).Succeeded());
	CHECK_FALSE(TraceRestrictSlot::Get(lane_id)->IsOccupant(slow->index));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}


TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - stable choices do not depend on slot registration order")
{
	ParallelBranch();
	Place(medium,14,8);medium->vehstatus.Set(VehState::Stopped);
	const bool reverse=GENERATE(false,true);
	if(reverse){Register(fast);Register(slow);}else{Register(slow);Register(fast);}
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	CHECK(GetRailReservationTrackBits(TileXY(14,8))==TRACK_BIT_NONE);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - both trains are clamped before threshold and waiting comparisons")
{
	const uint cap=GENERATE(1u,100u,124u,125u,160u,65535u);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|cap);
	Register(slow);Register(medium);
	const bool faster=cap>=125;
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster)==faster);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait)==faster);
	CHECK(((Execute(slow,10,10).flags&TRPRF_DENY)!=0)==faster);
	CHECK(slow->GetDisplayMaxSpeed()==100);CHECK(medium->GetDisplayMaxSpeed()==160);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - default rail limits are read live and zero is unlimited")
{
	const uint16_t cap=GENERATE(0,100,124,125,160,65535);
	AutoRestoreBackup<uint16_t> rail_limit(const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed,cap);
	Register(slow);Register(medium);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster)==(cap==0||cap>=125));
	// Rail conversion/NewGRF changes must not require re-saving the signal program.
	const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed=100;
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed=0;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - manual mode overrides the rail default")
{
	AutoRestoreBackup<uint16_t> rail_limit(const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed,80);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|160);
	Register(slow);Register(medium);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	Marker(3,10,AutoYieldRole::Approach);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - multiple detectors use the lowest cap independent of marker order")
{
	const bool reverse=GENERATE(false,true);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|(reverse?160:100));
	Marker(5,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|(reverse?100:160));
	Register(slow);Register(medium);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)==0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - equal clamped speeds cannot trigger eviction")
{
	Register(slow);Register(medium);Register(fast);
	Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);
	REQUIRE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|100);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,10,10).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(medium->index));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - commands preserve cap on edits and copies and reset it on role changes")
{
	const TileIndex tile=TileXY(3,10);
	const auto *program=GetExistingTraceRestrictProgram(tile,TRACK_X);
	const uint32_t config=(30u<<16)|160u;
	REQUIRE(CmdProgramSignalTraceRestrict({},tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,config,"").Succeeded());
	CHECK(program->items[1].base()==TEST_YIELD_CONFIG);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,config,"").Succeeded());
	CHECK(program->items[1].base()==config);
	REQUIRE(CmdProgramSignalTraceRestrictMgmt(DoCommandFlag::Execute,TileXY(10,10),TRACK_X,TRMDCT_PROG_COPY,tile,TRACK_X).Succeeded());
	CHECK(GetExistingTraceRestrictProgram(TileXY(10,10),TRACK_X)->items[1].base()==config);
	TraceRestrictInstructionItem item(program->items[0].base());
	item.SetAuxField(to_underlying(AutoYieldRole::Main));
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_ITEM,0,item.base(),"").Succeeded());
	item.SetAuxField(to_underlying(AutoYieldRole::Approach));
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_ITEM,0,item.base(),"").Succeeded());
	CHECK(program->items[1].base()==AUTO_YIELD_DEFAULT_CONFIG);
	CHECK(CmdProgramSignalTraceRestrict({},tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,(1001u<<16)|160,"").Failed());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield speed cap - clamped equality applies to multi-main shared exits")
{
	IndependentApproach();Place(fast,2,16);Register(slow);Register(fast);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|100);
	Place(slow,14,12);Execute(slow,10,12,TRPISP_ACQUIRE);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,18,12).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - mph and kmh input conversion does not leak into game state")
{
	const uint internal=160;
	const uint8_t unit=GENERATE(0,1,2,3);
	AutoRestoreBackup<uint8_t> units(_settings_game.locale.units_velocity,unit);
	AutoRestoreBackup new_units(_settings_newgame.locale.units_velocity,unit);
	const uint displayed=ConvertKmhishSpeedToDisplaySpeed(internal,VehicleType::Train);
	const uint round_trip=ConvertDisplaySpeedToKmhishSpeed(displayed,VehicleType::Train);
	CHECK(round_trip>0);CHECK(round_trip<=UINT16_MAX);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|internal);
	Register(slow);Register(medium);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	CHECK(GetExistingTraceRestrictProgram(TileXY(3,10),TRACK_X)->items[1].base()==(TEST_YIELD_CONFIG|160));
}


class AutomaticYieldPassengerFixture : public AutomaticYieldFixture {
protected:
	AutoRestoreBackup<CargoClasses> passenger_class{CargoSpec::Get(0)->classes,CargoClasses{CargoClass::Passengers}};
	AutoRestoreBackup<CargoClasses> freight_class{CargoSpec::Get(1)->classes,CargoClasses{CargoClass::Bulk}};
	void Load(Train *v, CargoType cargo=CargoType{0}) {
		v->cargo_type=cargo;v->cargo_cap=10;
		v->cargo.Append(CargoPacket::Create(uint16_t{1},uint16_t{0},StationID::Invalid(),TileXY(1,1),Money{0}));
	}
};

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield passenger priority - only equal effective speeds prefer loaded passenger trains")
{
	bool expected=true;
	SECTION("identical actual maximum speeds") { fast->vcache.cached_max_speed=100;Load(fast); }
	SECTION("identical clamped speeds") { Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|100);Load(fast); }
	SECTION("slower passenger train cannot outrank freight") { fast->vcache.cached_max_speed=80;Load(fast);expected=false; }
	SECTION("empty passenger train gets no special priority") { fast->vcache.cached_max_speed=100;fast->cargo_type=CargoType{0};fast->cargo_cap=10;expected=false; }
	SECTION("loaded freight gets no passenger priority") { fast->vcache.cached_max_speed=100;Load(fast,CargoType{1});expected=false; }
	SECTION("both trains carry passengers") { fast->vcache.cached_max_speed=100;Load(fast);Load(slow);expected=false; }
	Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==expected);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait)==expected);
	CHECK(((Execute(slow,10,10).flags&TRPRF_DENY)!=0)==expected);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield passenger priority - unloading is reflected in the next evaluation")
{
	fast->vcache.cached_max_speed=100;Load(fast);Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	fast->cargo.Truncate();
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	Load(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
}

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield passenger priority - payload on following parts counts")
{
	fast->vcache.cached_max_speed=100;
	Train *coach=MakeTrain();coach->SetWagon();coach->owner=fast->owner;fast->SetNext(coach);Load(coach);
	Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK_FALSE(EvaluateAutoYield(fast,membership->index.base(),AutoYieldCondition::MustWait));
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield defaults - new detectors use five percent and rail speed while existing values survive")
{
	CHECK(AUTO_YIELD_DEFAULT_CONFIG==(5u<<16));
	TraceRestrictInstructionItem item{};item.SetType(TRIT_AUTO_YIELD_AREA);item.SetAuxField(to_underlying(AutoYieldRole::Approach));item.SetValue(membership->index);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(10,10),TRACK_X,TRDCT_INSERT_ITEM,0,item.base(),"").Succeeded());
	CHECK(GetExistingTraceRestrictProgram(TileXY(10,10),TRACK_X)->items[1].base()==AUTO_YIELD_DEFAULT_CONFIG);
	CHECK(GetExistingTraceRestrictProgram(TileXY(3,10),TRACK_X)->items[1].base()==TEST_YIELD_CONFIG);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield defaults - five percent threshold is applied after the cap")
{
	const uint cap=GENERATE(100,104,105,160);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),AUTO_YIELD_DEFAULT_CONFIG|cap);
	Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==(cap>=105));
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - the detector uses its own track on mixed rail type tiles")
{
	AutoRestoreBackup<uint16_t> primary_limit(const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed,0);
	AutoRestoreBackup<uint16_t> secondary_limit(const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_ELECTRIC))->max_speed,100);
	const TileIndex tile=TileXY(3,20);
	MakeRailNormal(tile,_current_company,TRACK_BIT_HORZ,RAILTYPE_RAIL);SetSecondaryRailType(tile,RAILTYPE_ELECTRIC);
	SB(_m[tile].m5,6,2,to_underlying(RailTileType::Signals));
	SetPresentSignals(tile,SignalAlongTrackdir(TrackToTrackdir(TRACK_LOWER)));SetSignalType(tile,TRACK_LOWER,SignalType::Path);
	auto *p=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,TRACK_LOWER),true);
	TraceRestrictInstructionItem item{};item.SetType(TRIT_AUTO_YIELD_AREA);item.SetAuxField(to_underlying(AutoYieldRole::Approach));item.SetValue(membership->index);
	p->items={item.AsProgramItem(),TraceRestrictProgramItem{AUTO_YIELD_DEFAULT_CONFIG}};
	REQUIRE(p->Validate().Succeeded());InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
	Register(slow);Register(fast);
	CHECK(GetRailTypeByTrack(tile,TRACK_LOWER)==RAILTYPE_ELECTRIC);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - converting a single word action safely creates the default secondary word")
{
	const TileIndex tile=TileXY(10,10);
	auto *p=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,TRACK_X),false);
	TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items={deny.AsProgramItem()};p->items.shrink_to_fit();REQUIRE(p->Validate().Succeeded());
	TraceRestrictInstructionItem detector{};detector.SetType(TRIT_AUTO_YIELD_AREA);detector.SetAuxField(to_underlying(AutoYieldRole::Approach));detector.SetValue(membership->index);
	GameRandomSeedChecker random_checker;
	REQUIRE(CmdProgramSignalTraceRestrict({},tile,TRACK_X,TRDCT_MODIFY_ITEM,0,detector.base(),"").Succeeded());
	CHECK(p->items.size()==1);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_ITEM,0,detector.base(),"").Succeeded());
	REQUIRE(p->items.size()==2);
	CHECK(p->items[1].base()==AUTO_YIELD_DEFAULT_CONFIG);CHECK(p->Validate().Succeeded());CHECK(random_checker.Check());
}
