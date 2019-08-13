#include <torch/csrc/distributed/rpc/ProcessGroupAgent.h>

namespace torch {
namespace distributed {
namespace rpc {

namespace {

// Write the message into the given ostream
void serialize(const Message& message, std::ostream& os) {
  // We cast const void* to void* here because we need to create a tensor using
  // that memory space. If is fine as that tensor stays function-local, and will
  // not be modified during its lifetime.
  auto payload = const_cast<void*>(  // NOLINT
      static_cast<const void*>(message.payload().data()));
  auto payload_size = message.payload().size();

  // getting tensor table from the message
  std::vector<torch::Tensor> tensors = message.tensors();
  // append payload as a tensor
  tensors.push_back(torch::from_blob(payload, payload_size, {torch::kChar}));
  // append id and type as a tensor
  tensors.push_back(torch::tensor(
      {message.id(), (int64_t) message.type()}, {torch::kInt64}
  ));

  torch::save(tensors, os);
}

Message deserialize(std::istream& is) {
  std::vector<torch::Tensor> tensors;

  torch::load(tensors, is);

  TORCH_CHECK(tensors.size() >= 2, "Failed to deserialize a message.");
  auto miscTensor = std::move(tensors.back());
  tensors.pop_back();
  auto payloadTensor = std::move(tensors.back());
  tensors.pop_back();

  int64_t* miscItems = miscTensor.storage().data<int64_t>();
  int64_t id = miscItems[0];
  MessageType type = MessageType(miscItems[1]);

  std::vector<char> payload(payloadTensor.numel());

  if (payloadTensor.numel() > 0) {
    std::memcpy(payload.data(),
                payloadTensor.storage().data(),
                payloadTensor.numel());
  }

  return Message(std::move(payload), std::move(tensors), type, id);
}

std::unordered_map<std::string, int> collectNames(
    const std::string& workerName, std::shared_ptr<c10d::ProcessGroup>& pg) {

  const auto worldSize = pg->getSize();
  // collect lengths
  const int64_t length = workerName.length();
  std::vector<torch::Tensor> inputLength =
      { torch::tensor({length}, {torch::kInt64}) };
  std::vector<std::vector<torch::Tensor>> outputLengths (1);
  for (int i = 0; i < worldSize; ++i) {
    outputLengths[0].emplace_back(torch::empty({1}, {torch::kInt64}));
  }
  pg->allgather(outputLengths, inputLength)->wait();

  // convert collected length tensors into integers
  std::vector<int64_t> nameLengths(worldSize);
  int64_t maxLen = 0;
  for (int i = 0; i < worldSize; ++i) {
    nameLengths[i] = outputLengths[0][i].storage().data<int64_t>()[0];
    maxLen = std::max(maxLen, nameLengths[i]);
  }

  // collect names
  torch::Tensor nameTensor = torch::empty({maxLen}, torch::kChar);
  memcpy(nameTensor.storage().data(), workerName.c_str(), length);
  std::vector<torch::Tensor> inputName = {nameTensor};
  std::vector<std::vector<torch::Tensor>> outputNames(1);
  for (int i = 0; i < worldSize; ++i) {
    outputNames[0].emplace_back(torch::empty({maxLen}, {torch::kChar}));
  }
  pg->allgather(outputNames, inputName)->wait();

  // convert collected name tensors into string names
  std::unordered_map<std::string, int> nameMap(worldSize);
  for (int i = 0; i < worldSize; ++i) {
    torch::Tensor& tensor = outputNames[0][i];
    std::string peerName(
        (const char*)tensor.storage().data<signed char>(),
        nameLengths[i]
    );

    TORCH_CHECK(nameMap.find(peerName) == nameMap.end(),
        "RpcAgent name ", peerName, " is not unique.");

    nameMap[std::move(peerName)] = i;
  }

  return nameMap;
}

} // namespace

ProcessGroupAgent::ProcessGroupAgent(
    std::string workerName,
    std::shared_ptr<c10d::ProcessGroup> pg)
    : RpcAgent(std::move(workerName), pg->getRank(), processRequestBlocking),
      stop_(false),
      pg_(std::move(pg)),
      nextId_(0) {
  nameMap_ = collectNames(workerName_, pg_);
  TORCH_CHECK(nameMap_.size() > 1, "ProcessGroupAgent requires world_size to "
      "be at least 2, but got ", nameMap_.size());
  auto workerRankIter = nameMap_.find(workerName_);
  TORCH_CHECK(workerRankIter != nameMap_.end(), "Failed to resolve worker "
      "name ", workerName_, " to a ProcessGroup rank.");
  TORCH_CHECK(pg_->getRank() == workerRankIter -> second,
      "Resolved worker rank ", workerRankIter -> second,
      " does not match ProcessGroup rank ", pg_->getRank());

  sendThread_ = std::thread(&ProcessGroupAgent::sendLoop, this);
  listenerThread_ = std::thread(&ProcessGroupAgent::listenLoop, this);
}

worker_id_t ProcessGroupAgent::getId() {
  return id_;
}

worker_id_t ProcessGroupAgent::getWorkerId(const std::string& workerName) {
  const auto idIter = nameMap_.find(workerName);
  TORCH_CHECK(idIter != nameMap_.end(),
      "Unknown destination worker ", workerName);

  return idIter->second;
}

void ProcessGroupAgent::join() {
  // Every process i sends a SHUTDOWN message to process i + 1. This is
  // necessary for now because:
  // 1. There is no abort API for ProcessGroup::recvAnysource yet. We have to
  //    feed it a message or kill the thread.
  // 2. A GLOO process cannot send message to itself. (there is an ongoing
  //    effort to fix this problem).
  sync();
  int dst = (pg_->getRank() + 1) % pg_->getSize();
  enqueue(SendWork(dst, Message({}, {}, MessageType::SHUTDOWN)));
  std::unique_lock<std::mutex> lock(sendQueueMutex_);
  workConsumeCV_.wait(lock, [&] { return sendQueue_.empty(); });
  stop_ = true;
  lock.unlock();

  workProduceCV_.notify_all();
  sendThread_.join();
  listenerThread_.join();
}

void ProcessGroupAgent::sync() {
  // Block until all processes wants to sync. This is necessary before acquiring
  // the lock below, because other processes might not enter sync() until it
  // gets some response from this RpcAgent.
  pg_->barrier()->wait();
  // Acquire the lock on the send queue to prevent additional messages to be put
  // onto the send queue.
  std::unique_lock<std::mutex> lock(sendQueueMutex_);
  // Wait until the send queue is depleted.
  workConsumeCV_.wait(lock, [&] { return sendQueue_.empty(); });
  // Use another barrier in case different RpcAgent handles different amounts of
  // workloads.
  pg_->barrier()->wait();
}

std::shared_ptr<FutureMessage> ProcessGroupAgent::send(
    worker_id_t to, Message&& message) {
  TORCH_CHECK(to != (worker_id_t)pg_->getRank(),
      "ProcessGroupAgent does not support making RPC calls to self.")

  auto requestId = nextId();
  auto future = std::make_shared<FutureMessage>();
  if (message.isRequest()) {
    {
      std::lock_guard<std::mutex> lock{futureMutex_};
      futures_[requestId] = future;
    }
    message.setId(requestId);
  } else {
    future->markCompleted();
  }

  enqueue(SendWork(to, std::move(message)));
  return future;
}

void ProcessGroupAgent::enqueue(SendWork work) {
  std::unique_lock<std::mutex> lock(sendQueueMutex_);
  sendQueue_.emplace_back(std::move(work));
  lock.unlock();

  workProduceCV_.notify_one();
}

// making sure tensors are not deleted before send finishes
void ProcessGroupAgent::sendLoop() {
  std::unique_lock<std::mutex> lock(sendQueueMutex_);

  while (!stop_) {
    if (sendQueue_.empty()) {
      workProduceCV_.wait(lock);
      continue;
    }

    auto work = std::move(sendQueue_.front());
    sendQueue_.pop_front();
    lock.unlock();

    workConsumeCV_.notify_one();


    std::stringstream ss;
    serialize(work.message_, ss);
    std::string str = ss.str();

    std::vector<torch::Tensor> preamble = {
      torch::tensor(
        {
          (int64_t)pg_->getRank(),
          (int64_t)str.length(),
        }, {torch::kLong})
    };
    pg_->send(preamble, work.dstRank_, work.dstRank_ /* channelTag */)->wait();
    std::vector<torch::Tensor> payload =
        {torch::from_blob((void *)str.c_str(), str.length(), {torch::kChar})};
    pg_->send(payload, work.dstRank_, work.dstRank_ /* channelTag */)->wait();

    lock.lock();
  }
}

void ProcessGroupAgent::listenLoop() {
  while (true) {
    // rank, tensor size
    std::vector<torch::Tensor> preamble = {torch::empty({2}, {torch::kInt64})};
    pg_->recvAnysource(preamble, pg_->getRank())->wait();
    int64_t* preamble_items = preamble.front().storage().data<int64_t>();

    auto srcRank = preamble_items[0];
    auto size = preamble_items[1];

    std::vector<torch::Tensor> tensors = {torch::empty({size}, {torch::kChar})};
    pg_->recv(tensors, srcRank, pg_->getRank())->wait();

    std::stringstream ss(std::string(
      (char*)tensors[0].storage().data<signed char>(), tensors[0].numel()));

    Message message = deserialize(ss);

    if (message.isRequest()) {
      cb_(srcRank, std::move(message), *this);
    } else if (message.isResponse()) {
      auto id = message.id();
      {
        std::lock_guard<std::mutex> lock{futureMutex_};
        futures_[id]->markCompleted(std::move(message));
        futures_.erase(id);
      }
    } else if (message.isShutdown()) {
      break;
    } else {
      AT_ERROR("unrecognized message type ", message.type());
    }
  }
}

}
}
}
