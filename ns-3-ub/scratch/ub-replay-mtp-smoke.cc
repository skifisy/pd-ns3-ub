// SPDX-License-Identifier: GPL-2.0-only
// Synthetic collector regression, NOT an end-to-end network performance experiment.
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/unified-bus-module.h"
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

#ifdef NS3_MTP
namespace
{
constexpr uint32_t kLps=8, kPerBatch=2048, kPayload=1000, kWire=1064;

Ptr<UbPort> Port(Ptr<Node> node)
{
    auto port=CreateObject<UbPort>();
    port->SetAddress(Mac48Address::Allocate());
    node->AddDevice(port);
    return port;
}

void Connect(Ptr<UbPort> a, Ptr<UbPort> b)
{
    auto link=CreateObject<UbLink>();
    link->SetAttribute("Delay",TimeValue(NanoSeconds(1)));
    a->Attach(link);b->Attach(link);
}

void Batch(Ptr<UbPort> sourceFabric, uint32_t sourceNode, uint32_t destNode,
           uint32_t destLeaf, uint32_t lp, uint32_t batch)
{
    NS_ABORT_MSG_IF(Simulator::GetSystemId()!=lp,"Fixture executed on wrong LP");
    auto& logger=UbReplayLogger::Get();
    for (uint32_t i=0;i<kPerBatch;++i)
    {
        const uint32_t task=((lp-1)*3+batch)*kPerBatch+i;
        logger.TaskEvent("start","wqe",sourceNode,task);
        // Deliberately inject a successful-send counter observation. No protocol
        // receiver runs: this isolates sampler synchronization and CSV integrity.
        auto packet=Create<Packet>(kWire);
        UbReplayPathTag tag;
        tag.sourceNode=sourceNode;tag.destinationNode=destNode;
        tag.sourceLeaf=sourceFabric->GetNode()->GetId();tag.destinationLeaf=destLeaf;
        tag.payloadBytes=kPayload;packet->AddPacketTag(tag);
        sourceFabric->UpdateTxBytes(kWire);
        logger.SuccessfulTransmit(packet,tag.sourceLeaf,sourceFabric->GetIfIndex());
        logger.TaskEvent("complete","wqe",sourceNode,task);
    }
}

std::vector<std::string> Cells(const std::string& line)
{
    // The fixture uses fixed identifiers without commas or embedded quotes.
    std::vector<std::string> cells;
    std::istringstream input(line);std::string value;
    while (std::getline(input,value,','))
    {
        if (!value.empty() && value.back()=='\r') value.pop_back();
        if (value.size()>=2 && value.front()=='"' && value.back()=='"')
            value=value.substr(1,value.size()-2);
        cells.push_back(value);
    }
    return cells;
}

bool Verify(const std::filesystem::path& directory)
{
    std::map<uint32_t,uint64_t> wire,payload;
    std::map<uint32_t,uint8_t> tasks;
    for (const auto& name : {"replay_link_samples.csv","replay_node_path_samples.csv","replay_task_events.csv"})
    {
        std::ifstream input(directory/name);if (!input) return false;
        std::string line;std::getline(input,line);
        while (std::getline(input,line))
        {
            const auto cells=Cells(line);
            if (std::string(name)=="replay_task_events.csv")
            {
                if (cells.size()!=7) return false;
                const auto id=std::stoul(cells[6]);
                const uint8_t bit=cells[3]=="start"?1:cells[3]=="complete"?2:0;
                if (!bit || (tasks[id]&bit)) return false;
                tasks[id]|=bit;
                continue;
            }
            const bool link=std::string(name)=="replay_link_samples.csv";
            if (cells.size()!=(link?22:14)) return false;
            const auto sample=std::stoul(cells[0]);
            if (sample!=1 && sample!=2) return false;
            const auto end=std::stold(cells[link?4:3]);
            if (end!=(sample==1?500000000.0L:750000000.0L)) return false;
            const auto epoch=std::stoul(cells[link?11:5]);
            if (epoch!=(sample==1?0:1)) return false;
            (link?wire:payload)[sample]+=std::stoull(cells[link?17:12]);
        }
    }
    const uint64_t first=uint64_t(kLps)*kPerBatch*2,second=uint64_t(kLps)*kPerBatch;
    if (wire[1]!=first*kWire || wire[2]!=second*kWire ||
        payload[1]!=first*kPayload || payload[2]!=second*kPayload ||
        tasks.size()!=first+second) return false;
    for (const auto& [id,bits] : tasks)
    {
        (void)id;
        if (bits!=3) return false;
    }
    return true;
}
} // namespace
#endif

int main(int argc,char** argv)
{
#ifndef NS3_MTP
    (void)argc;(void)argv;
    std::cerr<<"Build with --enable-mtp before running this collector fixture\n";
    return 1;
#else
    std::string output="replay-mtp-smoke-output";
    CommandLine cmd;cmd.AddValue("output","Synthetic collector output directory",output);cmd.Parse(argc,argv);
    Time::SetResolution(Time::PS);
    MtpInterface::Enable(kLps,kLps);
    std::vector<NodeContainer> groups(kLps);
    std::vector<Ptr<UbPort>> outgoing;
    for (uint32_t lp=1;lp<=kLps;++lp)
    {
        auto& group=groups[lp-1];group.Create(4,lp);
        for (uint32_t i=0;i<4;++i)
        {
            auto sw=CreateObject<UbSwitch>();group.Get(i)->AggregateObject(sw);
            sw->SetNodeType(i==1 || i==2 ? UB_SWITCH : UB_DEVICE);
        }
        auto source=Port(group.Get(0)),sourceAccess=Port(group.Get(1));
        auto fabric=Port(group.Get(1)),destinationFabric=Port(group.Get(2));
        auto destinationAccess=Port(group.Get(2)),destination=Port(group.Get(3));
        Connect(source,sourceAccess);Connect(fabric,destinationFabric);Connect(destinationAccess,destination);
        outgoing.push_back(fabric);
    }
    auto& logger=UbReplayLogger::Get();
    logger.Configure(output,output,"ecmp","ecmp",MilliSeconds(500),false,false,true,true,"all",true);
    for (uint32_t lp=1;lp<=kLps;++lp)
        for (uint32_t batch=0;batch<3;++batch)
        {
            const auto& group=groups[lp-1];
            Simulator::ScheduleWithContext(group.Get(1)->GetId(),MilliSeconds(490+batch*10),
                &Batch,outgoing[lp-1],group.Get(0)->GetId(),group.Get(3)->GetId(),group.Get(2)->GetId(),lp,batch);
        }
    // Same-time public policy change: 500ms worker observations belong to epoch 0.
    MtpInterface::ScheduleGlobalAt(MilliSeconds(500),[](){UbReplayLogger::Get().TePolicyPublished(1,true);});
    Simulator::Stop(MilliSeconds(750));
    Simulator::Run();
    logger.Finish("synthetic-collector-smoke");
    const bool passed=Verify(output);
    std::cout<<"Synthetic 8-LP replay collector: "<<(passed?"PASSED":"FAILED")<<std::endl;
    Simulator::Destroy();
    return passed?0:1;
#endif
}
