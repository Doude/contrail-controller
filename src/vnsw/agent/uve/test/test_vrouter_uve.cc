/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <cfg/cfg_init.h>
#include <cfg/cfg_interface.h>
#include <oper/operdb_init.h>
#include <controller/controller_init.h>
#include <pkt/pkt_init.h>
#include <services/services_init.h>
#include <ksync/ksync_init.h>
#include <cmn/agent_cmn.h>
#include <base/task.h>
#include <io/event_manager.h>
#include <base/util.h>
#include <ifmap_agent_parser.h>
#include <ifmap_agent_table.h>
#include <oper/vn.h>
#include <oper/vm.h>
#include <oper/interface_common.h>
#include <oper/interface.h>
#include <oper/interface.h>
#include <oper/mirror_table.h>
#include <uve/agent_uve.h>

#include "testing/gunit.h"
#include "test_cmn_util.h"
#include "vr_types.h"
#include <uve/vrouter_stats_collector.h>
#include <uve/vrouter_uve_entry_test.h>
#include "uve/test/test_uve_util.h"

using namespace std;

void RouterIdDepInit() {
}

class VRouterStatsCollectorTask : public Task {
public:
    VRouterStatsCollectorTask(int count) : 
        Task((TaskScheduler::GetInstance()->GetTaskId("Agent::Uve")), 0), 
        count_(count) {
    }
    virtual bool Run() {
        for (int i = 0; i < count_; i++)
            Agent::GetInstance()->uve()->vrouter_stats_collector()->Run();
        return true;
    }
private:
    int count_;
};

class UveVrouterUveTest : public ::testing::Test {
public:
    void EnqueueVRouterStatsCollectorTask(int count) {
        TaskScheduler *scheduler = TaskScheduler::GetInstance();
        VRouterStatsCollectorTask *task = new VRouterStatsCollectorTask(count);
        scheduler->Enqueue(task);
    }
    TestUveUtil util_;
};


TEST_F(UveVrouterUveTest, VmAddDel) {
    VrouterUveEntryTest *vr = static_cast<VrouterUveEntryTest *>
        (Agent::GetInstance()->uve()->vrouter_uve_entry());
    vr->clear_count();

    const VrouterAgent &uve = vr->last_sent_vrouter();
    EXPECT_EQ(0U, vr->vrouter_msg_count());
    EXPECT_EQ(0U, uve.get_virtual_machine_list().size());

    util_.VmAdd(1);

    EXPECT_EQ(1U, vr->vrouter_msg_count());
    EXPECT_EQ(1U, uve.get_virtual_machine_list().size());

    util_.VmAdd(2);

    EXPECT_EQ(2U, vr->vrouter_msg_count());
    EXPECT_EQ(2U, uve.get_virtual_machine_list().size());

    util_.VmDelete(2);

    EXPECT_EQ(3U, vr->vrouter_msg_count());
    EXPECT_EQ(1U, uve.get_virtual_machine_list().size());

    util_.VmDelete(1);

    EXPECT_EQ(4U, vr->vrouter_msg_count());
    EXPECT_EQ(0U, uve.get_virtual_machine_list().size());
}

TEST_F(UveVrouterUveTest, VnAddDel) {
    VrouterUveEntryTest *vr = static_cast<VrouterUveEntryTest *>
        (Agent::GetInstance()->uve()->vrouter_uve_entry());
    vr->clear_count();

    const VrouterAgent &uve = vr->last_sent_vrouter();
    EXPECT_EQ(0U, vr->vrouter_msg_count());
    EXPECT_EQ(0U, uve.get_connected_networks().size());

    util_.VnAdd(1);

    EXPECT_EQ(1U, vr->vrouter_msg_count());
    EXPECT_EQ(1U, uve.get_connected_networks().size());

    util_.VnAdd(2);

    EXPECT_EQ(2U, vr->vrouter_msg_count());
    EXPECT_EQ(2U, uve.get_connected_networks().size());

    util_.VnDelete(2);

    EXPECT_EQ(3U, vr->vrouter_msg_count());
    EXPECT_EQ(1U, uve.get_connected_networks().size());

    util_.VnDelete(1);

    EXPECT_EQ(4U, vr->vrouter_msg_count());
    EXPECT_EQ(0U, uve.get_connected_networks().size());
}

TEST_F(UveVrouterUveTest, ComputeCpuState_1) {

    VrouterUveEntryTest *vr = static_cast<VrouterUveEntryTest *>
        (Agent::GetInstance()->uve()->vrouter_uve_entry());
    vr->clear_count();
    EnqueueVRouterStatsCollectorTask(1);
    client->WaitForIdle();
    EXPECT_EQ(0U, vr->compute_state_send_count());

    EnqueueVRouterStatsCollectorTask(5);
    client->WaitForIdle();
    EXPECT_EQ(1U, vr->compute_state_send_count());

    EnqueueVRouterStatsCollectorTask(1);
    client->WaitForIdle();
    EXPECT_EQ(1U, vr->compute_state_send_count());

    EnqueueVRouterStatsCollectorTask(5);
    client->WaitForIdle();
    EXPECT_EQ(2U, vr->compute_state_send_count());
}

int main(int argc, char **argv) {
    GETUSERARGS();
    client = TestInit(init_file, ksync_init);

    usleep(10000);
    int ret = RUN_ALL_TESTS();
    Agent::GetInstance()->GetEventManager()->Shutdown();
    AsioStop();
    return ret;
}
