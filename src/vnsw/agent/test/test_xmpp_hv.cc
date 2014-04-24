#include <test/test_basic_scale.h>

TEST_F(AgentBasicScaleTest, Basic) {
    client->Reset();
    client->WaitForIdle();

    XmppConnectionSetUp();
    BuildVmPortEnvironment();
	IpamInfo ipam_info[] = {
	    {"1.1.1.0", 24, "1.1.1.200"}
	};
    AddIPAM("vn1", ipam_info, 1);
    WAIT_FOR(1000, 10000, RouteFind("vrf1", Ip4Address::from_string("1.1.1.255"), 32));

    Ip4Address mc_addr = Ip4Address::from_string("255.255.255.255");
    WAIT_FOR(1000, 10000, MCRouteFind("vrf1", mc_addr));
    Ip4Address uc_addr = Ip4Address::from_string("1.1.1.1");
    WAIT_FOR(1000, 1000, RouteFind("vrf1", uc_addr, 32));
    const struct ether_addr *flood_mac = ether_aton("ff:ff:ff:ff:ff:ff");
    EXPECT_TRUE(L2RouteFind("vrf1", *flood_mac));
    const struct ether_addr *mac = ether_aton("00:00:00:00:01:01");
    EXPECT_TRUE(L2RouteFind("vrf1", *mac));

    VerifyVmPortActive(true);
    VerifyRoutes(false);
    mc_addr = Ip4Address::from_string("1.1.1.255");
    EXPECT_TRUE(RouteFind("vrf1", mc_addr, 32));
    
    //Delete vm-port and route entry in vrf1
    DelIPAM("vn1");
    WAIT_FOR(1000, 10000, !RouteFind("vrf1", Ip4Address::from_string("1.1.1.255"), 32));
    DeleteVmPortEnvironment();
}

TEST_F(AgentBasicScaleTest, multicast_one_channel_down_up) {
    client->Reset();
    client->WaitForIdle();

    XmppConnectionSetUp();
    BuildVmPortEnvironment();
	IpamInfo ipam_info[] = {
	    {"1.1.1.0", 24, "1.1.1.200"}
	};
    AddIPAM("vn1", ipam_info, 1);
    WAIT_FOR(1000, 10000, RouteFind("vrf1", Ip4Address::from_string("1.1.1.255"), 32));

    //expect subscribe message+route at the mock server
    Ip4Address mc_addr = Ip4Address::from_string("255.255.255.255");
    WAIT_FOR(1000, 10000, MCRouteFind("vrf1", mc_addr));
    client->WaitForIdle();
    mc_addr = Ip4Address::from_string("1.1.1.255");
    EXPECT_TRUE(RouteFind("vrf1", mc_addr, 32));
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(mcobj != NULL);
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() == subnet_src_label);
    EXPECT_TRUE((mcobj->peer_identifier() + 1) == 
                Agent::GetInstance()->controller()->multicast_peer_identifier());

    mc_addr = Ip4Address::from_string("255.255.255.255");
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    uint32_t source_flood_label = mcobj->GetSourceMPLSLabel();
    EXPECT_TRUE(MCRouteFind("vrf1", mc_addr));
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != 0);
    EXPECT_TRUE((mcobj->peer_identifier() + 1) == 
                Agent::GetInstance()->controller()->multicast_peer_identifier());

    //Bring up the channel
    bgp_peer[0].get()->HandleXmppChannelEvent(xmps::READY);
    VerifyConnections(0, 14);

    mc_addr = Ip4Address::from_string("1.1.1.255");
    EXPECT_TRUE(RouteFind("vrf1", mc_addr, 32));
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(mcobj != NULL);
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != 0);
    WAIT_FOR(1000, 1000, (mcobj->GetSourceMPLSLabel() != subnet_src_label));
    EXPECT_TRUE(mcobj->peer_identifier() == 
                Agent::GetInstance()->controller()->multicast_peer_identifier());
    mc_addr = Ip4Address::from_string("255.255.255.255");
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(MCRouteFind("vrf1", mc_addr));
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != 0);
    WAIT_FOR(1000, 1000, (mcobj->GetSourceMPLSLabel() != source_flood_label));
    EXPECT_TRUE(mcobj->peer_identifier() == 
                Agent::GetInstance()->controller()->multicast_peer_identifier());
    EXPECT_TRUE(old_multicast_identifier != 
                Agent::GetInstance()->controller()->multicast_peer_identifier());

    //Delete vm-port and route entry in vrf1
    DelIPAM("vn1");
    mc_addr = Ip4Address::from_string("1.1.1.255");
    WAIT_FOR(1000, 10000, !RouteFind("vrf1", Ip4Address::from_string("1.1.1.255"), 32));
    DeleteVmPortEnvironment();
}

TEST_F(AgentBasicScaleTest, multicast_one_channel_down_up_skip_route_from_peer) {
    client->Reset();
    client->WaitForIdle();

    XmppConnectionSetUp();
    BuildVmPortEnvironment();
	IpamInfo ipam_info[] = {
	    {"1.1.1.0", 24, "1.1.1.200"}
	};
    AddIPAM("vn1", ipam_info, 1);
    WAIT_FOR(1000, 10000, RouteFind("vrf1", Ip4Address::from_string("1.1.1.255"), 32));

    //expect subscribe message+route at the mock server
    Ip4Address mc_addr = Ip4Address::from_string("255.255.255.255");
    WAIT_FOR(1000, 10000, MCRouteFind("vrf1", mc_addr));

    VerifyVmPortActive(true);
    VerifyRoutes(false);
    mc_addr = Ip4Address::from_string("1.1.1.255");
    EXPECT_TRUE(RouteFind("vrf1", mc_addr, 32));
    
    //Store the src mpls label to verify it does not change after channel down
    MulticastGroupObject *mcobj = MulticastHandler::GetInstance()->
        FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(mcobj != NULL);

    uint32_t old_multicast_identifier = 
        Agent::GetInstance()->controller()->multicast_peer_identifier();
    WAIT_FOR(1000, 1000, (mcobj->GetSourceMPLSLabel() != 0));
    uint32_t subnet_src_label = mcobj->GetSourceMPLSLabel();

    //Bring down the channel
    AgentXmppChannel *ch = static_cast<AgentXmppChannel *>(bgp_peer[0].get());
    bgp_peer[0].get()->HandleXmppChannelEvent(xmps::NOT_READY);
    client->WaitForIdle();

    mc_addr = Ip4Address::from_string("1.1.1.255");
    EXPECT_TRUE(RouteFind("vrf1", mc_addr, 32));
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(mcobj != NULL);
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() == subnet_src_label);
    EXPECT_TRUE((mcobj->peer_identifier() + 1) == 
                Agent::GetInstance()->controller()->multicast_peer_identifier());

    uint32_t source_flood_label = mcobj->GetSourceMPLSLabel();
    mc_addr = Ip4Address::from_string("255.255.255.255");
    EXPECT_TRUE(MCRouteFind("vrf1", mc_addr));
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != 0);
    EXPECT_TRUE((mcobj->peer_identifier() + 1) == 
                Agent::GetInstance()->controller()->multicast_peer_identifier());

    //Bring up the channel
    mock_peer[0].get()->SkipRoute("1.1.1.255");
    bgp_peer[0].get()->HandleXmppChannelEvent(xmps::READY);
    VerifyConnections(0, 14);

    mc_addr = Ip4Address::from_string("1.1.1.255");
    EXPECT_TRUE(RouteFind("vrf1", mc_addr, 32));
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(mcobj != NULL);
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != 0);
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() == subnet_src_label);
    EXPECT_TRUE(mcobj->peer_identifier() == old_multicast_identifier); 
    mc_addr = Ip4Address::from_string("255.255.255.255");
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(MCRouteFind("vrf1", mc_addr));
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != 0);
    WAIT_FOR(1000, 1000, (mcobj->peer_identifier() == 
                Agent::GetInstance()->controller()->multicast_peer_identifier()));
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != source_flood_label);
    EXPECT_TRUE(old_multicast_identifier != 
                Agent::GetInstance()->controller()->multicast_peer_identifier());
    EXPECT_TRUE(Agent::GetInstance()->controller()->multicast_cleanup_timer()->running());

    //Fire the timer
    Agent::GetInstance()->controller()->multicast_cleanup_timer()->Fire();
    mc_addr = Ip4Address::from_string("1.1.1.255");
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(mcobj != NULL);
    WAIT_FOR(1000, 1000, (mcobj->GetSourceMPLSLabel() == 0));

    //Delete vm-port and route entry in vrf1
    DelIPAM("vn1");
    WAIT_FOR(1000, 10000, !RouteFind("vrf1", Ip4Address::from_string("1.1.1.255"), 32));
    DeleteVmPortEnvironment();
}

TEST_F(AgentBasicScaleTest, v4_unicast_one_channel_down_up) {
    client->Reset();
    client->WaitForIdle();

    XmppConnectionSetUp();
    BuildVmPortEnvironment();

    //expect subscribe message+route at the mock server
    Ip4Address uc_addr = Ip4Address::from_string("1.1.1.1");
    WAIT_FOR(1000, 10000, RouteFind("vrf1", uc_addr, 32));
    Inet4UnicastRouteEntry *rt = RouteGet("vrf1", uc_addr, 32);
    WAIT_FOR(1000, 10000, (rt->GetPathList().size() == 2));

    //Get the peer
    Peer *peer = Agent::GetInstance()->GetAgentXmppChannel(0)->bgp_peer_id();
    AgentPath *path = static_cast<AgentPath *>(rt->FindPath(peer));
    EXPECT_TRUE(path->is_stale() == false);

    VerifyVmPortActive(true);
    VerifyRoutes(false);
    
    //Bring down the channel
    AgentXmppChannel *ch = static_cast<AgentXmppChannel *>(bgp_peer[0].get());
    bgp_peer[0].get()->HandleXmppChannelEvent(xmps::NOT_READY);
    path = static_cast<AgentPath *>(rt->FindPath(peer));
    WAIT_FOR(1000, 1000, (path->is_stale()));
    EXPECT_TRUE(RouteFind("vrf1", uc_addr, 32));
    client->WaitForIdle();

    //Bring up the channel
    bgp_peer[0].get()->HandleXmppChannelEvent(xmps::READY);
    VerifyConnections(0, 12);
    WAIT_FOR(1000, 10000, (rt->GetPathList().size() == 3));
    EXPECT_TRUE(RouteFind("vrf1", uc_addr, 32));
    path = static_cast<AgentPath *>(rt->FindPath(peer));
    EXPECT_TRUE(path->is_stale());
    Peer *new_peer = Agent::GetInstance()->GetAgentXmppChannel(0)->bgp_peer_id();
    AgentPath *new_path = static_cast<AgentPath *>(rt->FindPath(new_peer));
    EXPECT_TRUE(new_path != path);
    EXPECT_TRUE(!new_path->is_stale());
    EXPECT_TRUE(path->is_stale());

    //Fire timer and verify stale path is gone
    Agent::GetInstance()->controller()->unicast_cleanup_timer()->Fire();
    WAIT_FOR(1000, 1000, (rt->FindPath(peer) == NULL));
    EXPECT_TRUE(rt->GetPathList().size() == 2);
    new_path = static_cast<AgentPath *>(rt->FindPath(new_peer));
    EXPECT_TRUE(!new_path->is_stale());

    //Delete vm-port and route entry in vrf1
    DeleteVmPortEnvironment();
}

TEST_F(AgentBasicScaleTest, DISABLED_unicast_one_channel_down_up_skip_route_from_peer) {
    client->Reset();
    client->WaitForIdle();

    XmppConnectionSetUp();
    BuildVmPortEnvironment();
	IpamInfo ipam_info[] = {
	    {"1.1.1.0", 24, "1.1.1.200"}
	};
    AddIPAM("vn1", ipam_info, 1);
    WAIT_FOR(1000, 10000, RouteFind("vrf1", Ip4Address::from_string("1.1.1.255"), 32));

    //expect subscribe message+route at the mock server
    Ip4Address mc_addr = Ip4Address::from_string("255.255.255.255");
    WAIT_FOR(1000, 10000, MCRouteFind("vrf1", mc_addr));
    //WAIT_FOR(100, 10000, (mock_peer[0].get()->Count() == 
    //                      ((6 * num_vns * num_vms_per_vn) + num_vns)));

    VerifyVmPortActive(true);
    VerifyRoutes(false);
    mc_addr = Ip4Address::from_string("1.1.1.255");
    EXPECT_TRUE(RouteFind("vrf1", mc_addr, 32));
    
    //Store the src mpls label to verify it does not change after channel down
    MulticastGroupObject *mcobj = MulticastHandler::GetInstance()->
        FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(mcobj != NULL);

    uint32_t old_multicast_identifier = 
        Agent::GetInstance()->controller()->multicast_peer_identifier();
    WAIT_FOR(1000, 1000, (mcobj->GetSourceMPLSLabel() != 0));
    uint32_t subnet_src_label = mcobj->GetSourceMPLSLabel();
    //EXPECT_TRUE(Agent::GetInstance()->GetMplsTable()->FindMplsLabel(subnet_src_label));

    //Bring down the channel
    AgentXmppChannel *ch = static_cast<AgentXmppChannel *>(bgp_peer[0].get());
    bgp_peer[0].get()->HandleXmppChannelEvent(xmps::NOT_READY);
    mc_addr = Ip4Address::from_string("1.1.1.255");
    EXPECT_TRUE(RouteFind("vrf1", mc_addr, 32));
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(mcobj != NULL);
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() == subnet_src_label);
    //EXPECT_TRUE(mcobj->peer_identifier() == 
    EXPECT_TRUE((mcobj->peer_identifier() + 1) == 
                Agent::GetInstance()->controller()->multicast_peer_identifier());
    //EXPECT_TRUE(Agent::GetInstance()->GetMplsTable()->FindMplsLabel(subnet_src_label));

    uint32_t source_flood_label = mcobj->GetSourceMPLSLabel();
    mc_addr = Ip4Address::from_string("255.255.255.255");
    EXPECT_TRUE(MCRouteFind("vrf1", mc_addr));
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != 0);
    //EXPECT_TRUE(mcobj->peer_identifier() == 
    EXPECT_TRUE((mcobj->peer_identifier() + 1) == 
                Agent::GetInstance()->controller()->multicast_peer_identifier());

    //Bring up the channel
    mock_peer[0].get()->SkipRoute("1.1.1.255");
    bgp_peer[0].get()->HandleXmppChannelEvent(xmps::READY);
    VerifyConnections(0, 14);

    mc_addr = Ip4Address::from_string("1.1.1.255");
    EXPECT_TRUE(RouteFind("vrf1", mc_addr, 32));
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(mcobj != NULL);
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != 0);
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() == subnet_src_label);
    EXPECT_TRUE(mcobj->peer_identifier() == old_multicast_identifier); 
    mc_addr = Ip4Address::from_string("255.255.255.255");
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(MCRouteFind("vrf1", mc_addr));
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != 0);
    WAIT_FOR(1000, 1000, (mcobj->peer_identifier() == 
                Agent::GetInstance()->controller()->multicast_peer_identifier()));
    EXPECT_TRUE(mcobj->GetSourceMPLSLabel() != source_flood_label);
    EXPECT_TRUE(old_multicast_identifier != 
                Agent::GetInstance()->controller()->multicast_peer_identifier());
    EXPECT_TRUE(Agent::GetInstance()->controller()->multicast_cleanup_timer()->running());

    //Fire the timer
    Agent::GetInstance()->controller()->multicast_cleanup_timer()->Fire();
    mc_addr = Ip4Address::from_string("1.1.1.255");
    mcobj = MulticastHandler::GetInstance()->FindGroupObject("vrf1", mc_addr);
    EXPECT_TRUE(mcobj != NULL);
    WAIT_FOR(1000, 1000, (mcobj->GetSourceMPLSLabel() == 0));

    //Delete vm-port and route entry in vrf1
    DelIPAM("vn1");
    WAIT_FOR(1000, 10000, !RouteFind("vrf1", Ip4Address::from_string("1.1.1.255"), 32));
    DeleteVmPortEnvironment();
}


int main(int argc, char **argv) {
    GETSCALEARGS();
    if (!headless_init) {
        return 0;
    } 

    if ((num_vns * num_vms_per_vn) > MAX_INTERFACES) {
        LOG(DEBUG, "Max interfaces is 200");
        return false;
    }
    if (num_ctrl_peers == 0 || num_ctrl_peers > MAX_CONTROL_PEER) {
        LOG(DEBUG, "Supported values - 1, 2");
        return false;
    }

    client = TestInit(init_file, ksync_init);
    Agent::GetInstance()->set_headless_agent_mode(true);
    InitXmppServers();

    int ret = RUN_ALL_TESTS();
    Agent::GetInstance()->GetEventManager()->Shutdown();
    AsioStop();
    return ret;
}
