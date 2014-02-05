/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_multicast.h"

#include <boost/bind.hpp>

#include "base/task_annotations.h"
#include "bgp/bgp_path.h"
#include "bgp/bgp_route.h"
#include "bgp/ipeer.h"
#include "bgp/inetmvpn/inetmvpn_table.h"
#include "bgp/routing-instance/routing_instance.h"

class McastTreeManager::DeleteActor : public LifetimeActor {
public:
    DeleteActor(McastTreeManager *tree_manager)
        : LifetimeActor(tree_manager->table_->routing_instance()->server()->
                lifetime_manager()),
          tree_manager_(tree_manager) {
    }
    virtual ~DeleteActor() {
    }

    virtual bool MayDelete() const {
        return tree_manager_->MayDelete();
    }

    virtual void Shutdown() {
        tree_manager_->Shutdown();
    }

    virtual void Destroy() {
        tree_manager_->table_->DestroyTreeManager();
    }

private:
    McastTreeManager *tree_manager_;
};

//
// Constructor for McastForwarder. We get the address of the forwarder
// from the nexthop. The RD also ought to contain the same information.
// The LabelBlockPtr from the InetMVpnRoute is copied for convenience.
//
McastForwarder::McastForwarder(InetMVpnRoute *route)
    : route_(route), label_(0), address_(0),
      rd_(route->GetPrefix().route_distinguisher()),
      router_id_(route->GetPrefix().router_id()) {
    const BgpPath *path = route->BestPath();
    if (route_->GetPrefix().type() == InetMVpnPrefix::NativeRoute) {
        level_ = McastTreeManager::LevelLocal;
        address_ = path->GetAttr()->nexthop().to_v4();
        label_block_ = path->GetAttr()->label_block();
    } else {
        level_ = McastTreeManager::LevelGlobal;
    }

    if (path->GetAttr()->ext_community())
        encap_ = path->GetAttr()->ext_community()->GetTunnelEncap();
}

//
// Destructor for McastForwarder. FLushes forward and reverse links to and
// from other McastForwarders.
//
McastForwarder::~McastForwarder() {
    FlushLinks();
    ReleaseLabel();
}

//
// Update the` McastForwarder based on information in the InetMVpnRoute.
// Return true if something changed.
//
bool McastForwarder::Update(InetMVpnRoute *route) {
    McastForwarder forwarder(route);
    bool changed = false;
    if (label_block_ != forwarder.label_block_) {
        label_block_ = forwarder.label_block_;
        changed = true;
    }
    if (address_ != forwarder.address_) {
        address_ = forwarder.address_;
        changed = true;
    }
    if (encap_ != forwarder.encap_) {
        encap_ = forwarder.encap_;
        changed = true;
    }

    return changed;
}

//
// Printable string for McastForwarder.
//
std::string McastForwarder::ToString() const {
    return rd_.ToString() + " -> " + integerToString(label_);
}

//
// Find a link to the given McastForwarder.
//
McastForwarder *McastForwarder::FindLink(McastForwarder *forwarder) {
    for (McastForwarderList::iterator it = tree_links_.begin();
         it != tree_links_.end(); ++it) {
        if (*it == forwarder) return forwarder;
    }
    return NULL;
}

//
// Add a link to the given McastForwarder.
//
void McastForwarder::AddLink(McastForwarder *forwarder) {
    assert(!FindLink(forwarder));
    tree_links_.push_back(forwarder);
}

//
// Remove a link to the given McastForwarder.
//
void McastForwarder::RemoveLink(McastForwarder *forwarder) {
    for (McastForwarderList::iterator it = tree_links_.begin();
         it != tree_links_.end(); ++it) {
        if (*it == forwarder) {
            tree_links_.erase(it);
            return;
        }
    }
}

//
// Flush all links from this McastForwarder.  Takes care of removing the
// reverse links as well.
//
void McastForwarder::FlushLinks() {
    for (McastForwarderList::iterator it = tree_links_.begin();
         it != tree_links_.end(); ++it) {
        (*it)->RemoveLink(this);
    }
    tree_links_.clear();
}

//
// Allocate a label for this McastForwarder.  The label gets allocated from
// the LabelBlock corresponding to the label range advertised by the peer.
// This is used when updating the distribution tree for the McastSGEntry to
// this McastForwarder belongs.
//
void McastForwarder::AllocateLabel() {
    label_ = label_block_->AllocateLabel();
}

//
// Release the label, if any, for this McastForwarder. This is required when
// updating the distribution tree for the McastSGEntry to which we belong.
void McastForwarder::ReleaseLabel() {
    if (label_ != 0) {
        label_block_->ReleaseLabel(label_);
        label_ = 0;
    }
}

//
// Construct an UpdateInfo with the RibOutAttr that needs to be advertised to
// the IPeer for the InetMVpnRoute associated with this McastForwarder. This
// is used the Export method of the InetMVpnTable.  The target RibPeerSet is
// in the UpdateInfo is assumed to be filled in by the caller.
//
// The main functionality here is to transform the McastForwarderList for the
// distribution tree into a BgpOList.
//
UpdateInfo *McastForwarder::GetUpdateInfo(InetMVpnTable *table) {
    CHECK_CONCURRENCY("db::DBTable");

    // Bail if the tree has not been
    if (tree_links_.empty() || label_ == 0)
        return NULL;

    BgpOList *olist = new BgpOList;
    for (McastForwarderList::const_iterator it = tree_links_.begin();
            it != tree_links_.end(); ++it) {
        BgpOListElem elem((*it)->address(), (*it)->label(), (*it)->encap());
        olist->elements.push_back(elem);
    }

    BgpAttrOList olist_attr = BgpAttrOList(olist);
    BgpAttrSpec attr_spec;
    attr_spec.push_back(&olist_attr);

    BgpServer *server = table->routing_instance()->server();
    BgpAttrPtr attr = server->attr_db()->Locate(attr_spec);
    UpdateInfo *uinfo = new UpdateInfo;
    uinfo->roattr = RibOutAttr(attr.get(), label_);
    return uinfo;
}

//
// Constructor for McastSGEntry.
//
McastSGEntry::McastSGEntry(McastManagerPartition *partition,
        Ip4Address group, Ip4Address source)
    : partition_(partition),
      group_(group),
      source_(source),
      forest_node_(NULL),
      cmcast_route_(NULL),
      on_work_queue_(false) {
    update_needed_[0] = false;
    update_needed_[1] = false;
}

//
// Destructor for McastSGEntry.
//
McastSGEntry::~McastSGEntry() {
}

//
// Printable string for McastSGEntry.
//
std::string McastSGEntry::ToString() const {
    return group_.to_string() + "," + source_.to_string();
}

//
// Add the given McastForwarder under this McastSGEntry and trigger update
// of the distribution tree.
//
void McastSGEntry::AddForwarder(McastForwarder *forwarder) {
    uint8_t level = forwarder->level();
    forwarder_lists_[level].insert(forwarder);
    update_needed_[level] = true;
    partition_->EnqueueSGEntry(this);
}

//
// Delete the given McastForwarder from this McastSGEntry and trigger update
// of the distribution tree.
//
void McastSGEntry::DeleteForwarder(McastForwarder *forwarder) {
    if (forwarder == forest_node_)
        DeleteCMcastRoute();
    uint8_t level = forwarder->level();
    forwarder_lists_[level].erase(forwarder);
    update_needed_[level] = true;
    partition_->EnqueueSGEntry(this);
}

void McastSGEntry::AddCMcastRoute() {
    assert(!forest_node_);
    assert(!cmcast_route_);

    uint8_t level = McastTreeManager::LevelLocal;
    ForwarderList *forwarders = &forwarder_lists_[level];
    if (forwarders->rbegin() == forwarders->rend())
        return;

    forest_node_ = *forwarders->rbegin();
    BgpTable *table = partition_->GetTreeManager()->table();
    BgpServer *server = table->routing_instance()->server();
    Ip4Address router_id(server->bgp_identifier());
    InetMVpnPrefix prefix(InetMVpnPrefix::CMcastRoute,
        RouteDistinguisher::null_rd, router_id, group_, source_);

    InetMVpnRoute rt_key(prefix);
    DBTablePartition *tbl_partition =
        static_cast<DBTablePartition *>(partition_->GetTablePartition());
    InetMVpnRoute *route =
        static_cast<InetMVpnRoute *>(tbl_partition->Find(&rt_key));
    if (!route) {
        route = new InetMVpnRoute(prefix);
        tbl_partition->Add(route);
    } else {
        route->ClearDelete();
    }

    BgpAttrSpec attr_spec;
    BgpAttrNextHop nexthop(server->bgp_identifier());
    attr_spec.push_back(&nexthop);
    BgpAttrSourceRd source_rd(
        RouteDistinguisher(forest_node_->route()->GetPrefix().route_distinguisher()));
    attr_spec.push_back(&source_rd);
    BgpAttrPtr attr = server->attr_db()->Locate(attr_spec);

    BgpPath *path = new BgpPath(0, BgpPath::Local, attr);
    route->InsertPath(path);
    tbl_partition->Notify(route);
    cmcast_route_ = route;
}

void McastSGEntry::DeleteCMcastRoute() {
    if (!cmcast_route_)
        return;

    assert(forest_node_);
    forest_node_ = NULL;
    DBTablePartition *tbl_partition =
        static_cast<DBTablePartition *>(partition_->GetTablePartition());
    cmcast_route_->RemovePath(BgpPath::Local);

    if (!cmcast_route_->BestPath()) {
        tbl_partition->Delete(cmcast_route_);
    } else {
        tbl_partition->Notify(cmcast_route_);
    }
    cmcast_route_ = NULL;
}

void McastSGEntry::UpdateCMcastRoute() {
    uint8_t level = McastTreeManager::LevelLocal;
    ForwarderList *forwarders = &forwarder_lists_[level];
    McastForwarder *new_forest_node;
    if (forwarders->rbegin() == forwarders->rend()) {
        new_forest_node = NULL;
    } else {
        new_forest_node = *forwarders->rbegin();
    }

    if (new_forest_node == forest_node_)
        return;

    DeleteCMcastRoute();
    AddCMcastRoute();
}

void McastSGEntry::UpdateRoutes(uint8_t level) {
    if (level == McastTreeManager::LevelLocal) {
        UpdateCMcastRoute();
    }
}

//
//
// Update the distribution tree for this McastSGEntry.  We traverse all the
// McastForwarders in sorted order and arrange them in breadth first fashion
// in a k-ary tree.  Building the tree in this manner guarantees that we get
// the same tree for a given set of forwarders, independent of the order in
// in which they joined. This predictability is deemed to be more important
// than other criteria such as minimizing disruption of traffic, minimizing
// the cost/weight of the tree etc.
//
void McastSGEntry::UpdateTree(uint8_t level) {
    CHECK_CONCURRENCY("db::DBTable");

    if (!update_needed_[level])
        return;
    if (level == McastTreeManager::LevelGlobal)
        return;

    int degree = McastTreeManager::kDegree;
    ForwarderList *forwarders = &forwarder_lists_[level];

    // First get rid of the previous distribution tree and enqueue all the
    // associated InetMVpnRoutes for notification.  Note that DBListeners
    // will not get invoked until after this routine is done.
    for (ForwarderList::iterator it = forwarders->begin();
         it != forwarders->end(); ++it) {
       (*it)->FlushLinks();
       (*it)->ReleaseLabel();
       partition_->GetTablePartition()->Notify((*it)->route());
    }

    // Create a vector of pointers to the McastForwarders in sorted order. We
    // resort to this because std::set doesn't support random access iterators.
    McastForwarderList vec;
    vec.reserve(forwarders->size());
    for (ForwarderList::iterator it = forwarders->begin();
         it != forwarders->end(); ++it) {
        vec.push_back(*it);
    }

    // Go through each McastForwarder in the vector and link it to it's parent
    // McastForwarder in the k-ary tree. We also add a link from the parent to
    // the entry in question.
    for (McastForwarderList::iterator it = vec.begin(); it != vec.end(); ++it) {
        (*it)->AllocateLabel();
        int idx = it - vec.begin();
        if (idx == 0)
            continue;

        int parent_idx = (idx - 1) / degree;
        McastForwarderList::iterator parent_it = vec.begin() + parent_idx;
        assert(parent_it != vec.end());
        (*it)->AddLink(*parent_it);
        (*parent_it)->AddLink(*it);
    }

    UpdateRoutes(level);
}

void McastSGEntry::UpdateTree() {
    for (uint8_t level = McastTreeManager::LevelFirst;
         level < McastTreeManager::LevelCount; ++level) {
        UpdateTree(level);
    }
}

bool McastSGEntry::empty() const {
    if (!forwarder_lists_[McastTreeManager::LevelLocal].empty())
        return false;
    if (!forwarder_lists_[McastTreeManager::LevelGlobal].empty())
        return false;
    return true;
}

//
// Constructor for McastManagerPartition.
//
McastManagerPartition::McastManagerPartition(McastTreeManager *tree_manager,
        size_t part_id)
    : tree_manager_(tree_manager),
      part_id_(part_id),
      update_count_(0),
      work_queue_(TaskScheduler::GetInstance()->GetTaskId("db::DBTable"),
              part_id_,
              boost::bind(&McastManagerPartition::ProcessSGEntry, this, _1)) {
}

//
// Destructor for McastManagerPartition.
//
McastManagerPartition::~McastManagerPartition() {
    work_queue_.Shutdown();
}

//
// Find the McastSGEntry for the given group and source.
//
McastSGEntry *McastManagerPartition::FindSGEntry(
        Ip4Address group, Ip4Address source) {
    McastSGEntry temp_sg_entry(this, group, source);
    SGList::iterator it = sg_list_.find(&temp_sg_entry);
    return (it != sg_list_.end() ? *it : NULL);
}

//
// Find or create the McastSGEntry for the given group and source.
//
McastSGEntry *McastManagerPartition::LocateSGEntry(
        Ip4Address group, Ip4Address source) {
    McastSGEntry *sg_entry = FindSGEntry(group, source);
    if (!sg_entry) {
        sg_entry = new McastSGEntry(this, group, source);
        sg_list_.insert(sg_entry);
    }
    return sg_entry;
}

//
// Enqueue the given McastSGEntry on the WorkQueue if it's not already on it.
//
void McastManagerPartition::EnqueueSGEntry(McastSGEntry *sg_entry) {
    if (sg_entry->on_work_queue())
        return;
    work_queue_.Enqueue(sg_entry);
    sg_entry->set_on_work_queue();
}

//
// Callback for the WorkQueue.  Get rid of the McastSGEntry if it there no
// McastForwarders under it. Otherwise, update the distribution tree for it.
//
bool McastManagerPartition::ProcessSGEntry(McastSGEntry *sg_entry) {
    CHECK_CONCURRENCY("db::DBTable");

    sg_entry->clear_on_work_queue();
    if (sg_entry->empty()) {
        sg_list_.erase(sg_entry);
        delete sg_entry;
    } else {
        sg_entry->UpdateTree();
        update_count_++;
    }

    if (sg_list_.empty())
        tree_manager_->MayResumeDelete();

    return true;
}

//
// Get the DBTablePartBase for the InetMVpnTable for our partition id.
//
DBTablePartBase *McastManagerPartition::GetTablePartition() {
    return tree_manager_->GetTablePartition(part_id_);
}

//
// Constructor for McastTreeManager.
//
McastTreeManager::McastTreeManager(InetMVpnTable *table)
    : table_(table), table_delete_ref_(this, table->deleter()) {
    deleter_.reset(new DeleteActor(this));
}

//
// Destructor for McastTreeManager.
//
McastTreeManager::~McastTreeManager() {
}

//
// Initialize the McastTreeManager. We allocate the McastManagerPartitions
// and register a DBListener for the InetMVpnTable.
//
void McastTreeManager::Initialize() {
    AllocPartitions();
    listener_id_ = table_->Register(
        boost::bind(&McastTreeManager::RouteListener, this, _1, _2));
}

//
// Terminate the McastTreeManager. We free the McastManagerPartitions
// and unregister from the InetMVpnTable.
//
void McastTreeManager::Terminate() {
    table_->Unregister(listener_id_);
    FreePartitions();
}

//
// Allocate the McastManagerPartitions.
//
void McastTreeManager::AllocPartitions() {
    for (int part_id = 0; part_id < DB::PartitionCount(); part_id++) {
        partitions_.push_back(new McastManagerPartition(this, part_id));
    }
}

//
// Free the McastManagerPartitions.
//
void McastTreeManager::FreePartitions() {
    for (size_t part_id = 0; part_id < partitions_.size(); part_id++) {
        delete partitions_[part_id];
    }
    partitions_.clear();
}

McastManagerPartition *McastTreeManager::GetPartition(int part_id) {
    return partitions_[part_id];
}

//
// Get the DBTablePartBase for the InetMVpnTable for given partition id.
//
DBTablePartBase *McastTreeManager::GetTablePartition(size_t part_id) {
    return table_->GetTablePartition(part_id);
}

//
// Construct export state for the given InetMVpnRoute. Note that the route
// only needs to be exported to the IPeer from which it was learnt.
//
UpdateInfo *McastTreeManager::GetUpdateInfo(InetMVpnRoute *route) {
    CHECK_CONCURRENCY("db::DBTable");

    DBState *dbstate = route->GetState(table_, listener_id_);
    McastForwarder *forwarder = dynamic_cast<McastForwarder *>(dbstate);

    if (!forwarder)
        return NULL;

    return forwarder->GetUpdateInfo(table_);
}

//
// DBListener callback handler for the InetMVpnTable. It creates or deletes
// the associated McastForwarder as appropriate. Also creates a McastSGEntry
// if one doesn't already exist.  However, McastSGEntrys don't get deleted
// from here.  They only get deleted from the WorkQueue callback routine.
//
void McastTreeManager::RouteListener(
        DBTablePartBase *tpart, DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");

    McastManagerPartition *partition = partitions_[tpart->index()];
    InetMVpnRoute *route = dynamic_cast<InetMVpnRoute *>(db_entry);

    DBState *dbstate = route->GetState(table_, listener_id_);
    if (!dbstate) {

        // We have no previous DBState for this route.
        // Bail if the route is deleted or has no best path.
        if (route->IsDeleted() || !route->BestPath())
            return;

        // Create a new McastForwarder and associate it with the route.
        McastSGEntry *sg_entry = partition->LocateSGEntry(
            route->GetPrefix().group(), route->GetPrefix().source());
        McastForwarder *forwarder = new McastForwarder(route);
        sg_entry->AddForwarder(forwarder);
        route->SetState(table_, listener_id_, forwarder);

    } else {

        McastSGEntry *sg_entry = partition->FindSGEntry(
            route->GetPrefix().group(), route->GetPrefix().source());
        assert(sg_entry);
        McastForwarder *forwarder = dynamic_cast<McastForwarder *>(dbstate);
        assert(forwarder);

        if (route->IsDeleted()) {

            // Delete the McastForwarder associated with the route.
            route->ClearState(table_, listener_id_);
            sg_entry->DeleteForwarder(forwarder);
            delete forwarder;

        } else if (forwarder->Update(route)) {

            // Trigger update of the distribution tree.
            partition->EnqueueSGEntry(sg_entry);
        }

    }
}

//
// Check if the McastTreeManager can be deleted. This can happen only if all
// the McastManagerPartitions are empty.
//
bool McastTreeManager::MayDelete() const {
    CHECK_CONCURRENCY("bgp::Config");

    for (PartitionList::const_iterator it = partitions_.begin();
         it != partitions_.end(); ++it) {
        if (!(*it)->empty())
            return false;
    }

    return true;
}

//
// Initiate shutdown for the McastTreeManager.
//
void McastTreeManager::Shutdown() {
    CHECK_CONCURRENCY("bgp::Config");
}

//
// Trigger deletion of the McastTreeManager and propagate the delete to any
// dependents.
//
void McastTreeManager::ManagedDelete() {
    deleter_->Delete();
}

//
// Attempt to enqueue a delete for the McastTreeManager.
//
void McastTreeManager::MayResumeDelete() {
    if (!deleter()->IsDeleted())
        return;

    BgpServer *server = table_->routing_instance()->server();
    server->lifetime_manager()->Enqueue(deleter());
}

//
// Return the LifetimeActor for the McastTreeManager.
//
LifetimeActor *McastTreeManager::deleter() {
    return deleter_.get();
}

