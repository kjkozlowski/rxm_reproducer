#include <cstring>
#include <cassert>

#include <iostream>
#include <thread>
#include <stdexcept>
#include <source_location>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <string>

extern "C" {
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_tagged.h>

}
#include <barrier>


namespace {

 fi_info* makeHints() {

  fi_info *hints = fi_allocinfo();
  if (!hints) {
    throw std::runtime_error("Hints allocation failed");
  }

  hints->domain_attr->threading = FI_THREAD_SAFE;
  hints->domain_attr->mr_mode =
      FI_MR_LOCAL | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_VIRT_ADDR;
  hints->domain_attr->name = nullptr;
  hints->fabric_attr->prov_name = strdup("verbs");

  hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
  hints->ep_attr->type = FI_EP_RDM;
  hints->ep_attr->protocol = FI_PROTO_RXM;
  hints->addr_format = FI_FORMAT_UNSPEC;
  hints->dest_addr = nullptr;
  hints->domain_attr->control_progress = FI_PROGRESS_AUTO;
  hints->domain_attr->data_progress = FI_PROGRESS_AUTO;
  hints->caps = FI_MSG |  FI_RMA | FI_TAGGED | FI_SOURCE |  FI_DIRECTED_RECV;
  hints->tx_attr->op_flags = FI_TRANSMIT_COMPLETE;

  return hints;
}

fi_cq_attr initCqAttrDefault() {
  fi_cq_attr attr{};
  // VERY important setting - without that the sync data sent with fi_(...)data
  // instructions (like fi_writedata) is not formatted properly and cannot be
  // read properly
  attr.format = FI_CQ_FORMAT_TAGGED;
  // These 2 settings are needed to use fi_cq_sread
  attr.wait_obj = FI_WAIT_UNSPEC;
  attr.wait_cond = FI_CQ_COND_NONE;
  return attr;
}

fi_av_attr initAvAttrDefault() {
  fi_av_attr avAttr{};
  std::memset(&avAttr, 0, sizeof(fi_av_attr));
  avAttr.type = FI_AV_MAP;
  return avAttr;
}

auto getAddr(fid *endpoint) {
  std::size_t addrLen = 0;
  auto ret = fi_getname(endpoint, nullptr, &addrLen);
  if ((ret != -FI_ETOOSMALL) || (addrLen <= 0)) {
    throw std::runtime_error("Unexpected error");
  }

  auto data = std::vector<char>(addrLen, 0);

  ret = fi_getname(endpoint, static_cast<void *>(data.data()), &addrLen);
  if (ret) {
    throw std::runtime_error("Unexpected error");
  }

  data.shrink_to_fit();
  return data;
}

void CHECK(int ret, const std::source_location location = std::source_location::current()) {
  if (ret) {
    std::ostringstream oss;
    oss << "Check failed file: " << location.file_name()
        << "(" << location.line() << ":" << location.column() << ") `"
        << location.function_name() << "`: " << fi_strerror(ret);

    throw std::runtime_error(oss.str());
  }
}

}

int main() {

static constexpr size_t bufferSize{2011000};
static constexpr uint64_t clientTag = 1;
static constexpr uint64_t serverTag = 2;

std::vector<char> serverAddr{};
std::vector<char> clientAddr{};
uint32_t serverAddrFormat = 0;
std::mutex serverDataMtx;
std::condition_variable serverCv;
std::mutex clientDataMtx;
std::condition_variable clientCv;

bool serverDataReady = false;
bool clientDataReady = false;

std::barrier send_barrier(2);
std::barrier recv_barrier(2);
std::barrier wait_send_barrier(2);
std::barrier wait_recv_barrier(2);
std::mutex mtx;

std::thread server([&]{
    fi_info *hints = makeHints();
    fi_info *info = nullptr;
    fid_fabric *fabric = nullptr;
    fid_domain *domain = nullptr;
    fid_ep *ep = nullptr;
    fid_cq *rxCq = nullptr;
    fid_cq *txCq = nullptr;
    fid_av *av = nullptr;

    auto rxCqAttr = initCqAttrDefault();
    auto txCqAtrr = initCqAttrDefault();
    auto avAttr = initAvAttrDefault();

    CHECK(fi_getinfo(fi_version(), nullptr, nullptr, 0,
                            hints, &info));
    CHECK(fi_fabric(info->fabric_attr, &fabric, nullptr));
    CHECK(fi_domain(fabric, info, &domain, nullptr));
    CHECK(fi_endpoint(domain, info, &ep, nullptr));
    CHECK(fi_cq_open(domain, &rxCqAttr, &rxCq, nullptr));
    CHECK(fi_cq_open(domain, &txCqAtrr, &txCq, nullptr));
    CHECK(fi_av_open(domain, &avAttr, &av, nullptr));

    CHECK(fi_ep_bind(ep, &txCq->fid, FI_TRANSMIT));
    CHECK(fi_ep_bind(ep, &rxCq->fid, FI_RECV));
    CHECK(fi_ep_bind(ep, &av->fid, 0));
    CHECK(fi_enable(ep));

    auto send_buf = malloc(bufferSize);
    auto recv_buf = malloc(bufferSize);
    std::size_t PP_MR_KEY = 10;
    fid_mr *send_mr = nullptr;
    fid_mr *recv_mr = nullptr;
    auto flags = FI_SEND | FI_RECV | FI_REMOTE_READ | FI_REMOTE_WRITE;
    fi_mr_reg(domain, send_buf, bufferSize, flags, 0, PP_MR_KEY, 0, &(send_mr), nullptr);
    fi_mr_reg(domain, recv_buf, bufferSize, flags, 0, PP_MR_KEY++, 0, &(recv_mr), nullptr);

    const auto localAddr = getAddr(&ep->fid);
    fi_addr_t localFiAddr = 0;

    if (info->domain_attr->caps & FI_LOCAL_COMM) {
        CHECK(fi_av_insert(av, localAddr.data(), 1, &localFiAddr, 0, nullptr) < 0);
    }

    {
        std::unique_lock lock(serverDataMtx);
        serverAddr = localAddr;
        serverAddrFormat = info->addr_format;
        serverDataReady = true;
        serverCv.notify_one();
    }

    std::unique_lock lock(clientDataMtx);
    clientCv.wait(lock, [&] { return clientDataReady; });

    fi_addr_t remoteFiAddr = 0;
    CHECK(fi_av_insert(av, clientAddr.data(), 1, &remoteFiAddr, 0, nullptr) < 0);

    while (fi_tsend(ep, send_buf, bufferSize, fi_mr_desc(send_mr), remoteFiAddr, serverTag, nullptr) != 0);


    send_barrier.arrive_and_wait();

    while (fi_trecv(ep, recv_buf, bufferSize, fi_mr_desc(recv_mr), remoteFiAddr, clientTag, 0, nullptr) != 0);

    recv_barrier.arrive_and_wait();

    fi_cq_err_entry comp{};

    {
      std::lock_guard lock {mtx};
      while (fi_cq_readfrom(txCq, &comp, 1, &comp.src_addr) < 0);
    }

    wait_send_barrier.arrive_and_wait();

    while (fi_cq_readfrom(rxCq, &comp, 1, &comp.src_addr) < 0);

    wait_recv_barrier.arrive_and_wait();

    fi_close(&send_mr->fid);
    fi_close(&recv_mr->fid);
    free(send_buf);
    free(recv_buf);
    fi_close(&ep->fid);
    fi_close(&av->fid);
    fi_close(&rxCq->fid);
    fi_close(&txCq->fid);
    fi_close(&domain->fid);
    fi_close(&fabric->fid);
    fi_freeinfo(info);
    fi_freeinfo(hints);
});

std::thread client([&]{
    fi_info *hints = makeHints();
    fi_info *info = nullptr;
    fid_fabric *fabric = nullptr;
    fid_domain *domain = nullptr;
    fid_ep *ep = nullptr;
    fid_cq *rxCq = nullptr;
    fid_cq *txCq = nullptr;
    fid_av *av = nullptr;
    auto rxCqAttr = initCqAttrDefault();
    auto txCqAtrr = initCqAttrDefault();
    auto avAttr = initAvAttrDefault();

    std::unique_lock lock(serverDataMtx);
    serverCv.wait(lock, [&] { return serverDataReady; });

    hints->dest_addrlen = serverAddr.size();
    hints->addr_format = serverAddrFormat;
    hints->dest_addr = serverAddr.data();

    CHECK(fi_getinfo(fi_version(), nullptr, nullptr, 0,
                            hints, &info));

    CHECK(fi_fabric(info->fabric_attr, &fabric, nullptr));
    CHECK(fi_domain(fabric, info, &domain, nullptr));
    CHECK(fi_endpoint(domain, info, &ep, nullptr));
    CHECK(fi_cq_open(domain, &rxCqAttr, &rxCq, nullptr));
    CHECK(fi_cq_open(domain, &txCqAtrr, &txCq, nullptr));
    CHECK(fi_av_open(domain, &avAttr, &av, nullptr));

    CHECK(fi_ep_bind(ep, &txCq->fid, FI_TRANSMIT));
    CHECK(fi_ep_bind(ep, &rxCq->fid, FI_RECV));
    CHECK(fi_ep_bind(ep, &av->fid, 0));
    CHECK(fi_enable(ep));

    auto send_buf = malloc(bufferSize);
    auto recv_buf = malloc(bufferSize);
    std::size_t PP_MR_KEY = 50;
    fid_mr *send_mr = nullptr;
    fid_mr *recv_mr = nullptr;
    auto flags = FI_SEND | FI_RECV;
    CHECK(fi_mr_reg(domain, recv_buf, bufferSize, flags, 0, PP_MR_KEY, 0, &(recv_mr), nullptr));
    CHECK(fi_mr_reg(domain, send_buf, bufferSize, flags, 0, PP_MR_KEY++, 0, &(send_mr), nullptr));

    const auto localAddr = getAddr(&ep->fid);

    {
        std::unique_lock lock(clientDataMtx);
        clientAddr = localAddr;
        clientDataReady = true;
        clientCv.notify_one();
    }

    fi_addr_t localFiAddr = 0;
    if (info->domain_attr->caps & FI_LOCAL_COMM) {
        auto ret = fi_av_insert(av, localAddr.data(), 1, &localFiAddr, 0, nullptr);
        CHECK(ret < 0);
        CHECK(ret != 1);
    }

    fi_addr_t remoteFiAddr = 0;
    CHECK(fi_av_insert(av, hints->dest_addr, 1, &remoteFiAddr, 0, nullptr) < 0);

    while (fi_tsend(ep, send_buf, bufferSize, fi_mr_desc(send_mr), remoteFiAddr, clientTag, nullptr) != 0);

    send_barrier.arrive_and_wait();

    while (fi_trecv(ep, recv_buf, bufferSize, fi_mr_desc(recv_mr), remoteFiAddr, serverTag, 0, nullptr) != 0);

    recv_barrier.arrive_and_wait();

    fi_cq_err_entry comp{};

    {
      std::lock_guard lock {mtx};
      while (fi_cq_readfrom(txCq, &comp, 1, &comp.src_addr) < 0);
    }

    wait_send_barrier.arrive_and_wait();

    while (fi_cq_readfrom(rxCq, &comp, 1, &comp.src_addr) < 0);

    wait_recv_barrier.arrive_and_wait();

    fi_close(&send_mr->fid);
    fi_close(&recv_mr->fid);
    free(send_buf);
    free(recv_buf);
    fi_close(&ep->fid);
    fi_close(&av->fid);
    fi_close(&rxCq->fid);
    fi_close(&txCq->fid);
    fi_close(&domain->fid);
    fi_close(&fabric->fid);
    fi_freeinfo(info);
    hints->dest_addr = nullptr;
    fi_freeinfo(hints);
});

server.join();
client.join();

}

