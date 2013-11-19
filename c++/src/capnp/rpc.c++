// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "rpc.h"
#include "capability-context.h"
#include <kj/debug.h>
#include <kj/vector.h>
#include <kj/async.h>
#include <kj/one-of.h>
#include <kj/function.h>
#include <unordered_map>
#include <map>
#include <queue>
#include <capnp/rpc.capnp.h>

namespace capnp {
namespace _ {  // private

namespace {

template <typename T>
inline constexpr uint messageSizeHint() {
  return 1 + sizeInWords<rpc::Message>() + sizeInWords<T>();
}
template <>
inline constexpr uint messageSizeHint<void>() {
  return 1 + sizeInWords<rpc::Message>();
}

kj::Maybe<kj::Array<PipelineOp>> toPipelineOps(List<rpc::PromisedAnswer::Op>::Reader ops) {
  auto result = kj::heapArrayBuilder<PipelineOp>(ops.size());
  for (auto opReader: ops) {
    PipelineOp op;
    switch (opReader.which()) {
      case rpc::PromisedAnswer::Op::NOOP:
        op.type = PipelineOp::NOOP;
        break;
      case rpc::PromisedAnswer::Op::GET_POINTER_FIELD:
        op.type = PipelineOp::GET_POINTER_FIELD;
        op.pointerIndex = opReader.getGetPointerField();
        break;
      default:
        // TODO(soon):  Handle better?
        KJ_FAIL_REQUIRE("Unsupported pipeline op.", (uint)opReader.which()) {
          return nullptr;
        }
    }
    result.add(op);
  }
  return result.finish();
}

Orphan<List<rpc::PromisedAnswer::Op>> fromPipelineOps(
    Orphanage orphanage, kj::ArrayPtr<const PipelineOp> ops) {
  auto result = orphanage.newOrphan<List<rpc::PromisedAnswer::Op>>(ops.size());
  auto builder = result.get();
  for (uint i: kj::indices(ops)) {
    rpc::PromisedAnswer::Op::Builder opBuilder = builder[i];
    switch (ops[i].type) {
      case PipelineOp::NOOP:
        opBuilder.setNoop();
        break;
      case PipelineOp::GET_POINTER_FIELD:
        opBuilder.setGetPointerField(ops[i].pointerIndex);
        break;
    }
  }
  return result;
}

kj::Exception toException(const rpc::Exception::Reader& exception) {
  kj::Exception::Nature nature =
      exception.getIsCallersFault()
          ? kj::Exception::Nature::PRECONDITION
          : kj::Exception::Nature::LOCAL_BUG;

  kj::Exception::Durability durability;
  switch (exception.getDurability()) {
    default:
    case rpc::Exception::Durability::PERMANENT:
      durability = kj::Exception::Durability::PERMANENT;
      break;
    case rpc::Exception::Durability::TEMPORARY:
      durability = kj::Exception::Durability::TEMPORARY;
      break;
    case rpc::Exception::Durability::OVERLOADED:
      durability = kj::Exception::Durability::OVERLOADED;
      break;
  }

  return kj::Exception(nature, durability, "(remote)", 0, kj::heapString(exception.getReason()));
}

void fromException(const kj::Exception& exception, rpc::Exception::Builder builder) {
  // TODO(someday):  Indicate the remote server name as part of the stack trace.
  builder.setReason(kj::str("remote exception: ", exception.getDescription()));
  builder.setIsCallersFault(exception.getNature() == kj::Exception::Nature::PRECONDITION);
  switch (exception.getDurability()) {
    case kj::Exception::Durability::PERMANENT:
      builder.setDurability(rpc::Exception::Durability::PERMANENT);
      break;
    case kj::Exception::Durability::TEMPORARY:
      builder.setDurability(rpc::Exception::Durability::TEMPORARY);
      break;
    case kj::Exception::Durability::OVERLOADED:
      builder.setDurability(rpc::Exception::Durability::OVERLOADED);
      break;
  }
}

// =======================================================================================

template <typename Id, typename T>
class ExportTable {
  // Table mapping integers to T, where the integers are chosen locally.

public:
  kj::Maybe<T&> find(Id id) {
    if (id < slots.size() && slots[id] != nullptr) {
      return slots[id];
    } else {
      return nullptr;
    }
  }

  bool erase(Id id) {
    if (id < slots.size() && slots[id] != nullptr) {
      slots[id] = T();
      freeIds.push(id);
      return true;
    } else {
      return false;
    }
  }

  T& next(Id& id) {
    if (freeIds.empty()) {
      id = slots.size();
      return slots.add();
    } else {
      id = freeIds.top();
      freeIds.pop();
      return slots[id];
    }
  }

  template <typename Func>
  void forEach(Func&& func) {
    for (Id i = 0; i < slots.size(); i++) {
      if (slots[i] != nullptr) {
        func(i, slots[i]);
      }
    }
  }

private:
  kj::Vector<T> slots;
  std::priority_queue<Id, std::vector<Id>, std::greater<Id>> freeIds;
};

template <typename Id, typename T>
class ImportTable {
  // Table mapping integers to T, where the integers are chosen remotely.

public:
  T& operator[](Id id) {
    if (id < kj::size(low)) {
      return low[id];
    } else {
      return high[id];
    }
  }

  kj::Maybe<T&> find(Id id) {
    if (id < kj::size(low)) {
      return low[id];
    } else {
      auto iter = high.find(id);
      if (iter == high.end()) {
        return nullptr;
      } else {
        return iter->second;
      }
    }
  }

  void erase(Id id) {
    if (id < kj::size(low)) {
      low[id] = T();
    } else {
      high.erase(id);
    }
  }

  template <typename Func>
  void forEach(Func&& func) {
    for (Id i: kj::indices(low)) {
      func(i, low[i]);
    }
    for (auto& entry: high) {
      func(entry.first, entry.second);
    }
  }

private:
  T low[16];
  std::unordered_map<Id, T> high;
};

// =======================================================================================

class RpcConnectionState final: public kj::TaskSet::ErrorHandler, public kj::Refcounted {
  class PromisedAnswerClient;

public:
  RpcConnectionState(const kj::EventLoop& eventLoop,
                     kj::Maybe<SturdyRefRestorerBase&> restorer,
                     kj::Own<VatNetworkBase::Connection>&& connection,
                     kj::Own<kj::PromiseFulfiller<void>>&& disconnectFulfiller)
      : eventLoop(eventLoop), restorer(restorer), connection(kj::mv(connection)),
        disconnectFulfiller(kj::mv(disconnectFulfiller)),
        tasks(eventLoop, *this) {
    tasks.add(messageLoop());
  }

  kj::Own<const ClientHook> restore(ObjectPointer::Reader objectId) {
    QuestionId questionId;
    kj::Own<QuestionRef> questionRef;
    auto paf = kj::newPromiseAndFulfiller<kj::Own<const RpcResponse>>(eventLoop);

    {
      auto lock = tables.lockExclusive();
      auto& question = lock->questions.next(questionId);

      // We need a dummy paramCaps since null normally indicates that the question has completed.
      question.paramCaps = kj::heap<CapInjectorImpl>(*this);

      questionRef = kj::refcounted<QuestionRef>(*this, questionId, kj::mv(paf.fulfiller));
      question.selfRef = *questionRef;

      paf.promise.attach(kj::addRef(*questionRef));
    }

    {
      auto message = connection->newOutgoingMessage(
          objectId.targetSizeInWords() + messageSizeHint<rpc::Restore>());

      auto builder = message->getBody().initAs<rpc::Message>().initRestore();
      builder.setQuestionId(questionId);
      builder.getObjectId().set(objectId);

      message->send();
    }

    auto pipeline = kj::refcounted<RpcPipeline>(
        *this, kj::mv(questionRef), eventLoop.fork(kj::mv(paf.promise)));

    return pipeline->getPipelinedCap(kj::Array<const PipelineOp>(nullptr));
  }

  void taskFailed(kj::Exception&& exception) override {
    {
      kj::Vector<kj::Own<const PipelineHook>> pipelinesToRelease;
      kj::Vector<kj::Own<const ClientHook>> clientsToRelease;
      kj::Vector<kj::Own<CapInjectorImpl>> paramCapsToRelease;

      auto lock = tables.lockExclusive();

      if (lock->networkException != nullptr) {
        // Oops, already disconnected.
        return;
      }

      kj::Exception networkException(
          kj::Exception::Nature::NETWORK_FAILURE, kj::Exception::Durability::PERMANENT,
          __FILE__, __LINE__, kj::str("Disconnected: ", exception.getDescription()));

      // All current questions complete with exceptions.
      lock->questions.forEach([&](QuestionId id, Question& question) {
        KJ_IF_MAYBE(questionRef, question.selfRef) {
          // QuestionRef still present.  Make sure it's not in the midst of being destroyed, then
          // reject it.
          KJ_IF_MAYBE(ownRef, kj::tryAddRef(*questionRef)) {
            questionRef->reject(kj::cp(networkException));
          }
        }
        KJ_IF_MAYBE(pc, question.paramCaps) {
          paramCapsToRelease.add(kj::mv(*pc));
        }
      });

      lock->answers.forEach([&](QuestionId id, Answer& answer) {
        KJ_IF_MAYBE(p, answer.pipeline) {
          pipelinesToRelease.add(kj::mv(*p));
        }

        KJ_IF_MAYBE(context, answer.callContext) {
          context->requestCancel();
        }
      });

      lock->exports.forEach([&](ExportId id, Export& exp) {
        clientsToRelease.add(kj::mv(exp.clientHook));
        exp = Export();
      });

      lock->imports.forEach([&](ExportId id, Import& import) {
        KJ_IF_MAYBE(f, import.promiseFulfiller) {
          f->get()->reject(kj::cp(networkException));
        }
      });

      lock->networkException = kj::mv(networkException);
    }

    {
      // Send an abort message.
      auto message = connection->newOutgoingMessage(
            messageSizeHint<rpc::Exception>() +
            (exception.getDescription().size() + 7) / sizeof(word));
      fromException(exception, message->getBody().getAs<rpc::Message>().initAbort());
      message->send();
    }

    // Indicate disconnect.
    disconnectFulfiller->fulfill();
  }

private:
  class ImportClient;
  class PromiseClient;
  class CapInjectorImpl;
  class CapExtractorImpl;
  class QuestionRef;
  class RpcPipeline;
  class RpcCallContext;
  class RpcResponse;

  // =======================================================================================
  // The Four Tables entry types
  //
  // We have to define these before we can define the class's fields.

  typedef uint32_t QuestionId;
  typedef uint32_t ExportId;

  struct Question {
    kj::Maybe<kj::Own<CapInjectorImpl>> paramCaps;
    // CapInjector from the parameter struct.  This will be released once the `Return` message is
    // received and `retainedCaps` processed.  (If this is non-null, then the call has not returned
    // yet.)

    kj::Maybe<QuestionRef&> selfRef;
    // The local QuestionRef, set to nullptr when it is destroyed, which is also when `Finish` is
    // sent.

    inline bool operator==(decltype(nullptr)) const {
      return paramCaps == nullptr && selfRef == nullptr;
    }
    inline bool operator!=(decltype(nullptr)) const { return !operator==(nullptr); }
  };

  struct Answer {
    bool active = false;
    // True from the point when the Call message is received to the point when both the `Finish`
    // message has been received and the `Return` has been sent.

    kj::Maybe<kj::Own<const PipelineHook>> pipeline;
    // Send pipelined calls here.  Becomes null as soon as a `Finish` is received.

    kj::Promise<void> asyncOp = kj::Promise<void>(nullptr);
    // Delete this promise to cancel the call.

    kj::Maybe<const RpcCallContext&> callContext;
    // The call context, if it's still active.  Becomes null when the `Return` message is sent.  This
    // object, if non-null, is owned by `asyncOp`.
  };

  struct Export {
    uint refcount = 0;
    // When this reaches 0, drop `clientHook` and free this export.

    kj::Own<const ClientHook> clientHook;

    inline bool operator==(decltype(nullptr)) const { return refcount == 0; }
    inline bool operator!=(decltype(nullptr)) const { return refcount != 0; }
  };

  struct Import {
    Import() = default;
    Import(const Import&) = delete;
    Import(Import&&) = default;
    Import& operator=(Import&&) = default;
    // If we don't explicitly write all this, we get some stupid error deep in STL.

    kj::Maybe<ImportClient&> client;
    // Becomes null when the import is destroyed.

    kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Own<const ClientHook>>>> promiseFulfiller;
    // If non-null, the import is a promise.
  };

  // =======================================================================================
  // OK, now we can define RpcConnectionState's member data.

  const kj::EventLoop& eventLoop;
  kj::Maybe<SturdyRefRestorerBase&> restorer;
  kj::Own<VatNetworkBase::Connection> connection;
  kj::Own<kj::PromiseFulfiller<void>> disconnectFulfiller;

  struct Tables {
    ExportTable<ExportId, Export> exports;
    ExportTable<QuestionId, Question> questions;
    ImportTable<QuestionId, Answer> answers;
    ImportTable<ExportId, Import> imports;
    // The order of the tables is important for correct destruction.

    std::unordered_map<const ClientHook*, ExportId> exportsByCap;
    // Maps already-exported ClientHook objects to their ID in the export table.

    kj::Maybe<kj::Exception> networkException;
    // If the connection has failed, this is the exception describing the failure.  All future
    // calls should throw this exception.
  };
  kj::MutexGuarded<Tables> tables;

  kj::TaskSet tasks;

  // =====================================================================================
  // ClientHook implementations

  class RpcClient: public ClientHook, public kj::Refcounted {
  public:
    RpcClient(const RpcConnectionState& connectionState)
        : connectionState(kj::addRef(connectionState)) {}

    virtual kj::Maybe<ExportId> writeDescriptor(
        rpc::CapDescriptor::Builder descriptor, Tables& tables) const = 0;
    // Writes a CapDescriptor referencing this client.  Must be called with the
    // RpcConnectionState's table locked -- a reference to them is passed as the second argument.
    // The CapDescriptor must be sent before unlocking the tables, as it may become invalid at
    // any time once the tables are unlocked.
    //
    // If writing the descriptor adds a new export to the export table, or increments the refcount
    // on an existing one, then the ID is returned and the caller is responsible for removing it
    // later.

    virtual kj::Maybe<kj::Own<const ClientHook>> writeTarget(
        rpc::Call::Target::Builder target) const = 0;
    // Writes the appropriate call target for calls to this capability and returns null.
    //
    // - OR -
    //
    // If calls have been redirected to some other local ClientHook, returns that hook instead.
    // This can happen if the capability represents a promise that has been resolved.

    // implements ClientHook -----------------------------------------

    VoidPromiseAndPipeline call(uint64_t interfaceId, uint16_t methodId,
                                kj::Own<CallContextHook>&& context) const override {
      // Implement call() by copying params and results messages.

      // We can and should propagate cancellation.
      context->allowAsyncCancellation();

      auto params = context->getParams();

      size_t sizeHint = params.targetSizeInWords();

      // TODO(perf):  Extend targetSizeInWords() to include a capability count?  Here we increase
      //   the size by 1/16 to deal with cap descriptors possibly expanding.  See also below, when
      //   handling the response, and in RpcRequest::send().
      sizeHint += sizeHint / 16;

      // Don't overflow.
      if (uint(sizeHint) != sizeHint) {
        sizeHint = ~uint(0);
      }

      auto request = newCall(interfaceId, methodId, sizeHint);

      request.set(context->getParams());
      context->releaseParams();

      auto promise = request.send();

      auto pipeline = promise.releasePipelineHook();

      auto voidPromise = promise.thenInAnyThread(kj::mvCapture(context,
          [](kj::Own<CallContextHook>&& context, Response<ObjectPointer> response) {
            size_t sizeHint = response.targetSizeInWords();

            // See above TODO.
            sizeHint += sizeHint / 16;

            // Don't overflow.
            if (uint(sizeHint) != sizeHint) {
              sizeHint = ~uint(0);
            }

            context->getResults(sizeHint).set(response);
          }));

      return { kj::mv(voidPromise), kj::mv(pipeline) };
    }

    kj::Own<const ClientHook> addRef() const override {
      return kj::addRef(*this);
    }
    const void* getBrand() const override {
      return &connectionState;
    }

  protected:
    kj::Own<const RpcConnectionState> connectionState;
  };

  class ImportClient final: public RpcClient {
    // A ClientHook that wraps an entry in the import table.

  public:
    ImportClient(const RpcConnectionState& connectionState, ExportId importId)
        : RpcClient(connectionState), importId(importId) {}

    ~ImportClient() noexcept(false) {
      {
        // Remove self from the import table, if the table is still pointing at us.  (It's possible
        // that another thread attempted to obtain this import just as the destructor started, in
        // which case that other thread will have constructed a new ImportClient and placed it in
        // the import table.)
        auto lock = connectionState->tables.lockExclusive();
        KJ_IF_MAYBE(import, lock->imports.find(importId)) {
          KJ_IF_MAYBE(i, import->client) {
            if (i == this) {
              lock->imports.erase(importId);
            }
          }
        }
      }

      // Send a message releasing our remote references.
      if (remoteRefcount > 0) {
        connectionState->sendReleaseLater(importId, remoteRefcount);
      }
    }

    kj::Maybe<kj::Own<ImportClient>> tryAddRemoteRef() {
      // Add a new RemoteRef and return a new ref to this client representing it.  Returns null
      // if this client is being deleted in another thread, in which case the caller should
      // construct a new one.

      KJ_IF_MAYBE(ref, kj::tryAddRef(*this)) {
        ++remoteRefcount;
        return kj::mv(*ref);
      } else {
        return nullptr;
      }
    }

    kj::Maybe<ExportId> writeDescriptor(
        rpc::CapDescriptor::Builder descriptor, Tables& tables) const override {
      descriptor.setReceiverHosted(importId);
      return nullptr;
    }

    kj::Maybe<kj::Own<const ClientHook>> writeTarget(
        rpc::Call::Target::Builder target) const override {
      target.setExportedCap(importId);
      return nullptr;
    }

    // implements ClientHook -----------------------------------------

    Request<ObjectPointer, ObjectPointer> newCall(
        uint64_t interfaceId, uint16_t methodId, uint firstSegmentWordSize) const override {
      auto request = kj::heap<RpcRequest>(
          *connectionState, firstSegmentWordSize, kj::addRef(*this));
      auto callBuilder = request->getCall();

      callBuilder.getTarget().setExportedCap(importId);
      callBuilder.setInterfaceId(interfaceId);
      callBuilder.setMethodId(methodId);

      auto root = request->getRoot();
      return Request<ObjectPointer, ObjectPointer>(root, kj::mv(request));
    }

    kj::Maybe<kj::Promise<kj::Own<const ClientHook>>> whenMoreResolved() const override {
      return nullptr;
    }

  private:
    ExportId importId;

    uint remoteRefcount = 0;
    // Number of times we've received this import from the peer.
  };

  class PipelineClient final: public RpcClient {
    // A ClientHook representing a pipelined promise.  Always wrapped in PromiseClient.

  public:
    PipelineClient(const RpcConnectionState& connectionState,
                   kj::Own<const QuestionRef>&& questionRef,
                   kj::Array<PipelineOp>&& ops)
        : RpcClient(connectionState), questionRef(kj::mv(questionRef)), ops(kj::mv(ops)) {}

   kj::Maybe<ExportId> writeDescriptor(
       rpc::CapDescriptor::Builder descriptor, Tables& tables) const override {
      auto promisedAnswer = descriptor.initReceiverAnswer();
      promisedAnswer.setQuestionId(questionRef->getId());
      promisedAnswer.adoptTransform(fromPipelineOps(
          Orphanage::getForMessageContaining(descriptor), ops));
      return nullptr;
    }

    kj::Maybe<kj::Own<const ClientHook>> writeTarget(
        rpc::Call::Target::Builder target) const override {
      auto builder = target.initPromisedAnswer();
      builder.setQuestionId(questionRef->getId());
      builder.adoptTransform(fromPipelineOps(Orphanage::getForMessageContaining(builder), ops));
      return nullptr;
    }

    // implements ClientHook -----------------------------------------

    Request<ObjectPointer, ObjectPointer> newCall(
        uint64_t interfaceId, uint16_t methodId, uint firstSegmentWordSize) const override {
      auto request = kj::heap<RpcRequest>(
          *connectionState, firstSegmentWordSize, kj::addRef(*this));
      auto callBuilder = request->getCall();

      callBuilder.setInterfaceId(interfaceId);
      callBuilder.setMethodId(methodId);

      auto root = request->getRoot();
      return Request<ObjectPointer, ObjectPointer>(root, kj::mv(request));
    }

    kj::Maybe<kj::Promise<kj::Own<const ClientHook>>> whenMoreResolved() const override {
      return nullptr;
    }

  private:
    kj::Own<const QuestionRef> questionRef;
    kj::Array<PipelineOp> ops;
  };

  class PromiseClient final: public RpcClient {
    // A ClientHook that initially wraps one client (in practice, an ImportClient or a
    // PipelineClient) and then, later on, redirects to some other client.

  public:
    PromiseClient(const RpcConnectionState& connectionState,
                  kj::Own<const ClientHook> initial,
                  kj::Promise<kj::Own<const ClientHook>> eventual)
        : RpcClient(connectionState),
          inner(kj::mv(initial)),
          fork(connectionState.eventLoop.fork(kj::mv(eventual))),
          resolveSelfPromise(connectionState.eventLoop.there(fork.addBranch(),
              [this](kj::Own<const ClientHook>&& resolution) {
                resolve(kj::mv(resolution));
              }, [this](kj::Exception&& exception) {
                resolve(newBrokenCap(kj::mv(exception)));
              })) {
      // Create a client that starts out forwarding all calls to `initial` but, once `eventual`
      // resolves, will forward there instead.  In addition, `whenMoreResolved()` will return a fork
      // of `eventual`.  Note that this means the application could hold on to `eventual` even after
      // the `PromiseClient` is destroyed; `eventual` must therefore make sure to hold references to
      // anything that needs to stay alive in order to resolve it correctly (such as making sure the
      // import ID is not released).
      resolveSelfPromise.eagerlyEvaluate(connectionState.eventLoop);
    }

    kj::Maybe<ExportId> writeDescriptor(
        rpc::CapDescriptor::Builder descriptor, Tables& tables) const override {
      auto cap = inner.lockExclusive()->get()->addRef();
      return connectionState->writeDescriptor(kj::mv(cap), descriptor, tables);
    }

    kj::Maybe<kj::Own<const ClientHook>> writeTarget(
        rpc::Call::Target::Builder target) const override {
      return connectionState->writeTarget(**inner.lockExclusive(), target);
    }

    // implements ClientHook -----------------------------------------

    Request<ObjectPointer, ObjectPointer> newCall(
        uint64_t interfaceId, uint16_t methodId, uint firstSegmentWordSize) const override {
      return inner.lockExclusive()->get()->newCall(interfaceId, methodId, firstSegmentWordSize);
    }

    kj::Maybe<kj::Promise<kj::Own<const ClientHook>>> whenMoreResolved() const override {
      return fork.addBranch();
    }

  private:
    kj::MutexGuarded<kj::Own<const ClientHook>> inner;
    kj::ForkedPromise<kj::Own<const ClientHook>> fork;

    // Keep this last, because the continuation uses *this, so it should be destroyed first to
    // ensure the continuation is not still running.
    kj::Promise<void> resolveSelfPromise;

    void resolve(kj::Own<const ClientHook> replacement) {
      // Careful to make sure the old client is not destroyed until we release the lock.
      kj::Own<const ClientHook> old;
      auto lock = inner.lockExclusive();
      old = kj::mv(*lock);
      *lock = replacement->addRef();
    }
  };

  kj::Maybe<ExportId> writeDescriptor(
      kj::Own<const ClientHook> cap, rpc::CapDescriptor::Builder descriptor,
      Tables& tables) const {
    // Write a descriptor for the given capability.  The tables must be locked by the caller and
    // passed in as a parameter.

    if (cap->getBrand() == this) {
      return kj::downcast<const RpcClient>(*cap).writeDescriptor(descriptor, tables);
    } else {
      auto iter = tables.exportsByCap.find(cap);
      if (iter != tables.exportsByCap.end()) {
        auto& exp = KJ_ASSERT_NONNULL(tables.exports.find(iter->second));
        ++exp.refcount;
        // TODO(now):  Check if it's a promise.
        descriptor.setSenderHosted(iter->second);
        return iter->second;
      } else {
        ExportId exportId;
        auto& exp = tables.exports.next(exportId);
        exp.refcount = 1;
        exp.clientHook = kj::mv(cap);
        descriptor.setSenderHosted(exportId);
        return exportId;
      }
    }
  }

  kj::Maybe<kj::Own<const ClientHook>> writeTarget(
      const ClientHook& cap, rpc::Call::Target::Builder target) const {
    // If calls to the given capability should pass over this connection, fill in `target`
    // appropriately for such a call and return nullptr.  Otherwise, return a `ClientHook` to which
    // the call should be forwarded; the caller should then delegate the call to that `ClientHook`.
    //
    // The main case where this ends up returning non-null is if `cap` is a promise that has
    // recently resolved.  The application might have started building a request before the promise
    // resolved, and so the request may have been built on the assumption that it would be sent over
    // this network connection, but then the promise resolved to point somewhere else before the
    // request was sent.  Now the request has to be redirected to the new target instead.

    if (cap.getBrand() == this) {
      return kj::downcast<const RpcClient>(cap).writeTarget(target);
    } else {
      return cap.addRef();
    }
  }

  // =====================================================================================
  // CapExtractor / CapInjector implementations

  class CapExtractorImpl final: public CapExtractor<rpc::CapDescriptor> {
    // Reads CapDescriptors from a received message.

  public:
    CapExtractorImpl(const RpcConnectionState& connectionState)
        : connectionState(connectionState) {}

    ~CapExtractorImpl() noexcept(false) {
      KJ_ASSERT(retainedCaps.getWithoutLock().size() == 0,
                "CapExtractorImpl destroyed without getting a chance to retain the caps!") {
        break;
      }
    }

    uint retainedListSizeHint(bool final) {
      // Get the expected size of the retained caps list, in words.  If `final` is true, then it
      // is known that no more caps will be extracted after this point, so an exact value can be
      // returned.  Otherwise, the returned size includes room for error.

      // If `final` is true then there's no need to lock.  If it is false, then asynchronous
      // access is possible.  It's probably not worth taking the lock to look; we'll just return
      // a silly estimate.
      uint count = final ? retainedCaps.getWithoutLock().size() : 32;
      return (count * sizeof(ExportId) + (sizeof(ExportId) - 1)) / sizeof(word);
    }

    Orphan<List<ExportId>> finalizeRetainedCaps(Orphanage orphanage) {
      // Called on finalization, when the lock is no longer needed.
      kj::Vector<ExportId> retainedCaps = kj::mv(this->retainedCaps.getWithoutLock());

      auto lock = connectionState.tables.lockExclusive();

      auto actualRetained = retainedCaps.begin();
      for (ExportId importId: retainedCaps) {
        // Check if the import still exists under this ID.
        KJ_IF_MAYBE(import, lock->imports.find(importId)) {
          KJ_IF_MAYBE(i, import->client) {
            if (i->tryAddRemoteRef() != nullptr) {
              // Import indeed still exists!  We are responsible for retaining it.
              // TODO(now):  Do we need to hold on to the ref that tryAddRemoteRef() returned?
              *actualRetained++ = importId;
            }
          }
        }
      }

      uint count = actualRetained - retainedCaps.begin();

      // Build the retain list out of the imports that had non-zero refcounts.
      auto result = orphanage.newOrphan<List<ExportId>>(count);
      auto resultBuilder = result.get();
      count = 0;
      for (auto iter = retainedCaps.begin(); iter < actualRetained; ++iter) {
        resultBuilder.set(count++, *iter);
      }

      return kj::mv(result);
    }

    // implements CapDescriptor ------------------------------------------------

    kj::Own<const ClientHook> extractCap(rpc::CapDescriptor::Reader descriptor) const override {
      switch (descriptor.which()) {
        case rpc::CapDescriptor::SENDER_HOSTED:
        case rpc::CapDescriptor::SENDER_PROMISE: {
          ExportId importId = descriptor.getSenderHosted();

          auto lock = connectionState.tables.lockExclusive();

          auto& import = lock->imports[importId];
          KJ_IF_MAYBE(i, import.client) {
            // The import is already on the table, but it could be being deleted in another
            // thread.
            KJ_IF_MAYBE(ref, kj::tryAddRef(*i)) {
              // We successfully grabbed a reference to the import without it being deleted in
              // another thread.  Since this import already exists, we don't have to take
              // responsibility for retaining it.  We can just return the existing object and
              // be done with it.
              return kj::mv(*ref);
            }
          }

          // No import for this ID exists currently, so create one.
          kj::Own<ImportClient> importClient =
              kj::refcounted<ImportClient>(connectionState, importId);
          import.client = *importClient;

          kj::Own<ClientHook> result;
          if (descriptor.which() == rpc::CapDescriptor::SENDER_PROMISE) {
            // TODO(now):  Check for pending `Resolve` messages replacing this import ID, and if
            //   one exists, use that client instead.

            auto paf = kj::newPromiseAndFulfiller<kj::Own<const ClientHook>>();
            import.promiseFulfiller = kj::mv(paf.fulfiller);
            paf.promise.attach(kj::addRef(*importClient));
            result = kj::refcounted<PromiseClient>(
                connectionState, kj::mv(importClient), kj::mv(paf.promise));
          } else {
            result = kj::mv(importClient);
          }

          // Note that we need to retain this import later if it still exists.
          retainedCaps.lockExclusive()->add(importId);

          return kj::mv(result);
        }

        case rpc::CapDescriptor::RECEIVER_HOSTED: {
          auto lock = connectionState.tables.lockExclusive();  // TODO(perf): shared?
          KJ_IF_MAYBE(exp, lock->exports.find(descriptor.getReceiverHosted())) {
            return exp->clientHook->addRef();
          }
          return newBrokenCap("invalid 'receiverHosted' export ID");
        }

        case rpc::CapDescriptor::RECEIVER_ANSWER: {
          auto lock = connectionState.tables.lockExclusive();
          auto promisedAnswer = descriptor.getReceiverAnswer();
          KJ_IF_MAYBE(answer, lock->answers.find(promisedAnswer.getQuestionId())) {
            if (answer->active) {
              KJ_IF_MAYBE(pipeline, answer->pipeline) {
                KJ_IF_MAYBE(ops, toPipelineOps(promisedAnswer.getTransform())) {
                  return pipeline->get()->getPipelinedCap(*ops);
                } else {
                  return newBrokenCap("unrecognized pipeline ops");
                }
              }
            }
          }

          return newBrokenCap("invalid 'receiverAnswer'");
        }

        case rpc::CapDescriptor::THIRD_PARTY_HOSTED:
          return newBrokenCap("three-way introductions not implemented");

        default:
          return newBrokenCap("unknown CapDescriptor type");
      }
    }

  private:
    const RpcConnectionState& connectionState;

    kj::MutexGuarded<kj::Vector<ExportId>> retainedCaps;
    // Imports which we are responsible for retaining, should they still exist at the time that
    // this message is released.
  };

  // -----------------------------------------------------------------

  class CapInjectorImpl final: public CapInjector<rpc::CapDescriptor> {
    // Write CapDescriptors into a message as it is being built, before sending it.

  public:
    CapInjectorImpl(const RpcConnectionState& connectionState)
        : connectionState(connectionState) {}
    ~CapInjectorImpl() noexcept(false) {
      kj::Vector<kj::Own<const ClientHook>> clientsToRelease(exports.size());

      auto lock = connectionState.tables.lockExclusive();

      if (lock->networkException == nullptr) {
        for (auto exportId: exports) {
          KJ_DBG(&connectionState, exportId);
          auto& exp = KJ_ASSERT_NONNULL(lock->exports.find(exportId));
          if (--exp.refcount == 0) {
            clientsToRelease.add(kj::mv(exp.clientHook));
            lock->exports.erase(exportId);
          }
        }
      }
    }

    bool hasCaps() {
      // Return true if the message contains any capabilities.  (If not, it may be possible to
      // release earlier.)

      return !caps.getWithoutLock().empty();
    }

    void finishDescriptors(Tables& tables) {
      // Finish writing all of the CapDescriptors.  Must be called with the tables locked, and the
      // message must be sent before the tables are unlocked.

      exports = kj::Vector<ExportId>(caps.getWithoutLock().size());

      for (auto& entry: caps.getWithoutLock()) {
        // If maybeExportId is inlined, GCC 4.7 reports a spurious "may be used uninitialized"
        // error (GCC 4.8 and Clang do not complain).
        auto maybeExportId = connectionState.writeDescriptor(
            entry.second.cap->addRef(), entry.second.builder, tables);
        KJ_IF_MAYBE(exportId, maybeExportId) {
          KJ_ASSERT(tables.exports.find(*exportId) != nullptr);
          exports.add(*exportId);
        }
      }
    }

    // implements CapInjector ----------------------------------------

    void injectCap(rpc::CapDescriptor::Builder descriptor,
                   kj::Own<const ClientHook>&& cap) const override {
      auto lock = caps.lockExclusive();
      auto result = lock->insert(std::make_pair(
          identity(descriptor), CapInfo(descriptor, kj::mv(cap))));
      KJ_REQUIRE(result.second, "A cap has already been injected at this location.") {
        break;
      }
    }
    kj::Own<const ClientHook> getInjectedCap(rpc::CapDescriptor::Reader descriptor) const override {
      auto lock = caps.lockExclusive();
      auto iter = lock->find(identity(descriptor));
      KJ_REQUIRE(iter != lock->end(), "getInjectedCap() called on descriptor I didn't write.");
      return iter->second.cap->addRef();
    }
    void dropCap(rpc::CapDescriptor::Reader descriptor) const override {
      caps.lockExclusive()->erase(identity(descriptor));
    }

  private:
    const RpcConnectionState& connectionState;

    struct CapInfo {
      rpc::CapDescriptor::Builder builder;
      kj::Own<const ClientHook> cap;

      CapInfo(): builder(nullptr) {}

      CapInfo(rpc::CapDescriptor::Builder& builder, kj::Own<const ClientHook>&& cap)
          : builder(builder), cap(kj::mv(cap)) {}

      CapInfo(const CapInfo& other) = delete;
      // Work around problem where std::pair complains about the copy constructor requiring a
      // non-const argument due to `builder` inheriting kj::DisableConstCopy.  The copy constructor
      // should be deleted anyway because `cap` is not copyable.

      CapInfo(CapInfo&& other) = default;
    };

    kj::MutexGuarded<std::map<const void*, CapInfo>> caps;
    // Maps CapDescriptor locations to embedded caps.  The descriptors aren't actually filled in
    // until just before the message is sent.

    kj::Vector<ExportId> exports;
    // IDs of objects exported during finishDescriptors().  These will need to be released later.

    static const void* identity(const rpc::CapDescriptor::Reader& desc) {
      // TODO(cleanup):  Don't rely on internal APIs here.
      return _::PointerHelpers<rpc::CapDescriptor>::getInternalReader(desc).getLocation();
    }
  };

  // =====================================================================================
  // RequestHook/PipelineHook/ResponseHook implementations

  class QuestionRef: public kj::Refcounted {
    // A reference to an entry on the question table.  Used to detect when the `Finish` message
    // can be sent.

  public:
    inline QuestionRef(const RpcConnectionState& connectionState, QuestionId id,
                       kj::Own<kj::PromiseFulfiller<kj::Own<const RpcResponse>>> fulfiller)
        : connectionState(kj::addRef(connectionState)), id(id), fulfiller(kj::mv(fulfiller)),
          resultCaps(connectionState) {}

    ~QuestionRef() {
      // Send the "Finish" message.
      auto message = connectionState->connection->newOutgoingMessage(
          messageSizeHint<rpc::Finish>() + resultCaps.retainedListSizeHint(true));
      auto builder = message->getBody().getAs<rpc::Message>().initFinish();
      builder.setQuestionId(id);

      builder.adoptRetainedCaps(resultCaps.finalizeRetainedCaps(
          Orphanage::getForMessageContaining(builder)));

      message->send();

      // Check if the question has returned and, if so, remove it from the table.
      // Remove question ID from the table.  Must do this *after* sending `Finish` to ensure that
      // the ID is not re-allocated before the `Finish` message can be sent.
      {
        auto lock = connectionState->tables.lockExclusive();
        auto& question = KJ_ASSERT_NONNULL(
            lock->questions.find(id), "Question ID no longer on table?");
        if (question.paramCaps == nullptr) {
          // Call has already returned, so we can now remove it from the table.
          KJ_ASSERT(lock->questions.erase(id));
        } else {
          question.selfRef = nullptr;
        }
      }
    }

    inline QuestionId getId() const { return id; }
    inline CapExtractorImpl& getCapExtractor() { return resultCaps; }

    void fulfill(kj::Own<const RpcResponse>&& response) {
      fulfiller->fulfill(kj::mv(response));
    }

    void reject(kj::Exception&& exception) {
      fulfiller->reject(kj::mv(exception));
    }

  private:
    kj::Own<const RpcConnectionState> connectionState;
    QuestionId id;
    kj::Own<kj::PromiseFulfiller<kj::Own<const RpcResponse>>> fulfiller;
    CapExtractorImpl resultCaps;
  };

  class RpcRequest final: public RequestHook {
  public:
    RpcRequest(const RpcConnectionState& connectionState, uint firstSegmentWordSize,
               kj::Own<const RpcClient>&& target)
        : connectionState(kj::addRef(connectionState)),
          target(kj::mv(target)),
          message(connectionState.connection->newOutgoingMessage(
              firstSegmentWordSize == 0 ? 0 : firstSegmentWordSize + messageSizeHint<rpc::Call>())),
          injector(kj::heap<CapInjectorImpl>(connectionState)),
          context(*injector),
          callBuilder(message->getBody().getAs<rpc::Message>().initCall()),
          paramsBuilder(context.imbue(callBuilder.getParams())) {}

    inline ObjectPointer::Builder getRoot() {
      return paramsBuilder;
    }
    inline rpc::Call::Builder getCall() {
      return callBuilder;
    }

    RemotePromise<ObjectPointer> send() override {
      QuestionId questionId;
      kj::Own<QuestionRef> questionRef;
      kj::Promise<kj::Own<const RpcResponse>> promise = nullptr;

      {
        auto lock = connectionState->tables.lockExclusive();

        KJ_IF_MAYBE(e, lock->networkException) {
          return RemotePromise<ObjectPointer>(
              kj::Promise<Response<ObjectPointer>>(kj::cp(*e)),
              ObjectPointer::Pipeline(newBrokenPipeline(kj::cp(*e))));
        }

        KJ_IF_MAYBE(redirect, target->writeTarget(callBuilder.getTarget())) {
          // Whoops, this capability has been redirected while we were building the request!
          // We'll have to make a new request and do a copy.  Ick.

          lock.release();

          size_t sizeHint = paramsBuilder.targetSizeInWords();

          // TODO(perf):  See TODO in RpcClient::call() about why we need to inflate the size a bit.
          sizeHint += sizeHint / 16;

          // Don't overflow.
          if (uint(sizeHint) != sizeHint) {
            sizeHint = ~uint(0);
          }

          auto replacement = redirect->get()->newCall(
              callBuilder.getInterfaceId(), callBuilder.getMethodId(), sizeHint);
          replacement.set(paramsBuilder);
          return replacement.send();
        } else {
          injector->finishDescriptors(*lock);

          auto paf = kj::newPromiseAndFulfiller<kj::Own<const RpcResponse>>(
              connectionState->eventLoop);
          auto& question = lock->questions.next(questionId);

          callBuilder.setQuestionId(questionId);
          question.paramCaps = kj::mv(injector);

          questionRef = kj::refcounted<QuestionRef>(
              *connectionState, questionId, kj::mv(paf.fulfiller));
          question.selfRef = *questionRef;

          message->send();

          promise = kj::mv(paf.promise);
          promise.attach(kj::addRef(*questionRef));
        }
      }

      auto forkedPromise = connectionState->eventLoop.fork(kj::mv(promise));

      auto appPromise = forkedPromise.addBranch().thenInAnyThread(
          [](kj::Own<const RpcResponse>&& response) {
            auto reader = response->getResults();
            return Response<ObjectPointer>(reader, kj::mv(response));
          });

      auto pipeline = kj::refcounted<RpcPipeline>(
          *connectionState, kj::mv(questionRef), kj::mv(forkedPromise));

      return RemotePromise<ObjectPointer>(
          kj::mv(appPromise),
          ObjectPointer::Pipeline(kj::mv(pipeline)));
    }

  private:
    kj::Own<const RpcConnectionState> connectionState;

    kj::Own<const RpcClient> target;
    kj::Own<OutgoingRpcMessage> message;
    kj::Own<CapInjectorImpl> injector;
    CapBuilderContext context;
    rpc::Call::Builder callBuilder;
    ObjectPointer::Builder paramsBuilder;
  };

  class RpcPipeline final: public PipelineHook, public kj::Refcounted {
  public:
    RpcPipeline(const RpcConnectionState& connectionState, kj::Own<const QuestionRef> questionRef,
                kj::ForkedPromise<kj::Own<const RpcResponse>>&& redirectLaterParam)
        : connectionState(kj::addRef(connectionState)),
          redirectLater(kj::mv(redirectLaterParam)),
          resolveSelfPromise(connectionState.eventLoop.there(redirectLater.addBranch(),
              [this](kj::Own<const RpcResponse>&& response) {
                resolve(kj::mv(response));
              }, [this](kj::Exception&& exception) {
                resolve(kj::mv(exception));
              })) {
      // Construct a new RpcPipeline.

      resolveSelfPromise.eagerlyEvaluate(connectionState.eventLoop);
      state.getWithoutLock().init<Waiting>(kj::mv(questionRef));
    }

    kj::Promise<kj::Own<const RpcResponse>> onResponse() const {
      return redirectLater.addBranch();
    }

    // implements PipelineHook ---------------------------------------

    kj::Own<const PipelineHook> addRef() const override {
      return kj::addRef(*this);
    }

    kj::Own<const ClientHook> getPipelinedCap(kj::ArrayPtr<const PipelineOp> ops) const override {
      auto copy = kj::heapArrayBuilder<PipelineOp>(ops.size());
      for (auto& op: ops) {
        copy.add(op);
      }
      return getPipelinedCap(copy.finish());
    }

    kj::Own<const ClientHook> getPipelinedCap(kj::Array<PipelineOp>&& ops) const override {
      auto lock = state.lockExclusive();
      if (lock->is<Waiting>()) {
        // Wrap a PipelineClient in a PromiseClient.
        auto pipelineClient = kj::refcounted<PipelineClient>(
            *connectionState, kj::addRef(*lock->get<Waiting>()), kj::heapArray(ops.asPtr()));

        auto resolutionPromise = connectionState->eventLoop.there(redirectLater.addBranch(),
            kj::mvCapture(ops,
              [](kj::Array<PipelineOp> ops, kj::Own<const RpcResponse>&& response) {
                return response->getResults().getPipelinedCap(ops);
              }));

        return kj::refcounted<PromiseClient>(
            *connectionState, kj::mv(pipelineClient), kj::mv(resolutionPromise));
      } else if (lock->is<Resolved>()) {
        return lock->get<Resolved>()->getResults().getPipelinedCap(ops);
      } else {
        return newBrokenCap(kj::cp(lock->get<Broken>()));
      }
    }

  private:
    kj::Own<const RpcConnectionState> connectionState;
    kj::Maybe<CapExtractorImpl&> capExtractor;
    kj::ForkedPromise<kj::Own<const RpcResponse>> redirectLater;

    typedef kj::Own<const QuestionRef> Waiting;
    typedef kj::Own<const RpcResponse> Resolved;
    typedef kj::Exception Broken;
    kj::MutexGuarded<kj::OneOf<Waiting, Resolved, Broken>> state;

    // Keep this last, because the continuation uses *this, so it should be destroyed first to
    // ensure the continuation is not still running.
    kj::Promise<void> resolveSelfPromise;

    void resolve(kj::Own<const RpcResponse>&& response) {
      auto lock = state.lockExclusive();
      KJ_ASSERT(lock->is<Waiting>(), "Already resolved?");
      lock->init<Resolved>(kj::mv(response));
    }

    void resolve(const kj::Exception&& exception) {
      auto lock = state.lockExclusive();
      KJ_ASSERT(lock->is<Waiting>(), "Already resolved?");
      lock->init<Broken>(kj::mv(exception));
    }
  };

  class RpcResponse final: public ResponseHook, public kj::Refcounted {
  public:
    RpcResponse(const RpcConnectionState& connectionState,
                kj::Own<QuestionRef>&& questionRef,
                kj::Own<IncomingRpcMessage>&& message,
                ObjectPointer::Reader results)
        : connectionState(kj::addRef(connectionState)),
          message(kj::mv(message)),
          context(questionRef->getCapExtractor()),
          reader(context.imbue(results)),
          questionRef(kj::mv(questionRef)) {}

    ObjectPointer::Reader getResults() const {
      return reader;
    }

    kj::Own<const RpcResponse> addRef() const {
      return kj::addRef(*this);
    }

  private:
    kj::Own<const RpcConnectionState> connectionState;
    kj::Own<IncomingRpcMessage> message;
    CapReaderContext context;
    ObjectPointer::Reader reader;
    kj::Own<const QuestionRef> questionRef;
  };

  // =====================================================================================
  // CallContextHook implementation

  class RpcServerResponse {
  public:
    RpcServerResponse(const RpcConnectionState& connectionState,
                      kj::Own<OutgoingRpcMessage>&& message,
                      ObjectPointer::Builder results)
        : connectionState(connectionState),
          message(kj::mv(message)),
          injector(connectionState),
          context(injector),
          builder(context.imbue(results)) {}

    ObjectPointer::Builder getResults() {
      return builder;
    }

    bool hasCaps() {
      return injector.hasCaps();
    }

    void send() {
      auto lock = connectionState.tables.lockExclusive();
      injector.finishDescriptors(*lock);
      message->send();
    }

  private:
    const RpcConnectionState& connectionState;
    kj::Own<OutgoingRpcMessage> message;
    CapInjectorImpl injector;
    CapBuilderContext context;
    ObjectPointer::Builder builder;
  };

  class RpcCallContext final: public CallContextHook, public kj::Refcounted {
  public:
    RpcCallContext(const RpcConnectionState& connectionState, QuestionId questionId,
                   kj::Own<IncomingRpcMessage>&& request, const ObjectPointer::Reader& params)
        : connectionState(kj::addRef(connectionState)),
          questionId(questionId),
          request(kj::mv(request)),
          requestCapExtractor(connectionState),
          requestCapContext(requestCapExtractor),
          params(requestCapContext.imbue(params)),
          returnMessage(nullptr) {}

    void sendReturn() {
      if (isFirstResponder()) {
        if (response == nullptr) getResults(1);  // force initialization of response

        returnMessage.setQuestionId(questionId);
        returnMessage.adoptRetainedCaps(requestCapExtractor.finalizeRetainedCaps(
            Orphanage::getForMessageContaining(returnMessage)));

        KJ_ASSERT_NONNULL(response)->send();
      }
    }
    void sendErrorReturn(kj::Exception&& exception) {
      if (isFirstResponder()) {
        auto message = connectionState->connection->newOutgoingMessage(
            messageSizeHint<rpc::Return>() + sizeInWords<rpc::Exception>() +
            exception.getDescription().size() / sizeof(word) + 1);
        auto builder = message->getBody().initAs<rpc::Message>().initReturn();

        builder.setQuestionId(questionId);
        builder.adoptRetainedCaps(requestCapExtractor.finalizeRetainedCaps(
            Orphanage::getForMessageContaining(builder)));
        fromException(exception, builder.initException());

        message->send();
      }
    }
    void sendCancel() {
      if (isFirstResponder()) {
        auto message = connectionState->connection->newOutgoingMessage(
            messageSizeHint<rpc::Return>());
        auto builder = message->getBody().initAs<rpc::Message>().initReturn();

        builder.setQuestionId(questionId);
        builder.adoptRetainedCaps(requestCapExtractor.finalizeRetainedCaps(
            Orphanage::getForMessageContaining(builder)));
        builder.setCanceled();

        message->send();
      }
    }

    void requestCancel() const {
      // Hints that the caller wishes to cancel this call.  At the next time when cancellation is
      // deemed safe, the RpcCallContext shall send a canceled Return -- or if it never becomes
      // safe, the RpcCallContext will send a normal return when the call completes.  Either way
      // the RpcCallContext is now responsible for cleaning up the entry in the answer table, since
      // a Finish message was already received.

      // Verify that we're holding the tables mutex.  This is important because we're handing off
      // responsibility for deleting the answer.  Moreover, the callContext pointer in the answer
      // table should not be null as this would indicate that we've already returned a result.
      KJ_DASSERT(connectionState->tables.getAlreadyLockedExclusive()
                     .answers[questionId].callContext != nullptr);

      if (__atomic_fetch_or(&cancellationFlags, CANCEL_REQUESTED, __ATOMIC_RELAXED) ==
          CANCEL_ALLOWED) {
        // We just set CANCEL_REQUESTED, and CANCEL_ALLOWED was already set previously.  Schedule
        // the cancellation.
        scheduleCancel();
      }
    }

    // implements CallContextHook ------------------------------------

    ObjectPointer::Reader getParams() override {
      KJ_REQUIRE(request != nullptr, "Can't call getParams() after releaseParams().");
      return params;
    }
    void releaseParams() override {
      request = nullptr;
    }
    ObjectPointer::Builder getResults(uint firstSegmentWordSize) override {
      KJ_IF_MAYBE(r, response) {
        return r->get()->getResults();
      } else {
        auto message = connectionState->connection->newOutgoingMessage(
            firstSegmentWordSize == 0 ? 0 :
            firstSegmentWordSize + messageSizeHint<rpc::Return>() +
            requestCapExtractor.retainedListSizeHint(request == nullptr));
        returnMessage = message->getBody().initAs<rpc::Message>().initReturn();
        auto response = kj::heap<RpcServerResponse>(
            *connectionState, kj::mv(message), returnMessage.getResults());
        auto results = response->getResults();
        this->response = kj::mv(response);
        return results;
      }
    }
    void allowAsyncCancellation() override {
      if (threadAcceptingCancellation != nullptr) {
        threadAcceptingCancellation = &kj::EventLoop::current();

        if (__atomic_fetch_or(&cancellationFlags, CANCEL_ALLOWED, __ATOMIC_RELAXED) ==
            CANCEL_REQUESTED) {
          // We just set CANCEL_ALLOWED, and CANCEL_REQUESTED was already set previously.  Schedule
          // the cancellation.
          scheduleCancel();
        }
      }
    }
    bool isCanceled() override {
      return __atomic_load_n(&cancellationFlags, __ATOMIC_RELAXED) & CANCEL_REQUESTED;
    }
    kj::Own<CallContextHook> addRef() override {
      return kj::addRef(*this);
    }

  private:
    kj::Own<const RpcConnectionState> connectionState;
    QuestionId questionId;

    // Request ---------------------------------------------

    kj::Maybe<kj::Own<IncomingRpcMessage>> request;
    CapExtractorImpl requestCapExtractor;
    CapReaderContext requestCapContext;
    ObjectPointer::Reader params;

    // Response --------------------------------------------

    kj::Maybe<kj::Own<RpcServerResponse>> response;
    rpc::Return::Builder returnMessage;
    bool responseSent = false;

    // Cancellation state ----------------------------------

    enum CancellationFlags {
      CANCEL_REQUESTED = 1,
      CANCEL_ALLOWED = 2
    };

    mutable uint8_t cancellationFlags = 0;
    // When both flags are set, the cancellation process will begin.  Must be manipulated atomically
    // as it may be accessed from multiple threads.

    mutable kj::Promise<void> deferredCancellation = nullptr;
    // Cancellation operation scheduled by cancelLater().  Must only be scheduled once, from one
    // thread.

    kj::EventLoop* threadAcceptingCancellation = nullptr;
    // EventLoop for the thread that first called allowAsyncCancellation().  We store this as an
    // optimization:  if the application thread is independent from the network thread, we'd rather
    // perform the cancellation in the application thread, because otherwise we might block waiting
    // on an application promise continuation callback to finish executing, which could take
    // arbitrary time.

    // -----------------------------------------------------

    void scheduleCancel() const {
      // Arranges for the answer's asyncOp to be deleted, thus canceling all processing related to
      // this call, shortly.  We have to do it asynchronously because the caller might hold
      // arbitrary locks or might in fact be part of the task being canceled.

      deferredCancellation = threadAcceptingCancellation->evalLater([this]() {
        // Make sure we don't accidentally delete ourselves in the process of canceling, since the
        // last reference to the context may be owned by the asyncOp.
        auto self = kj::addRef(*this);

        // Extract from the answer table the promise representing the executing call.
        kj::Promise<void> asyncOp = nullptr;
        {
          auto lock = connectionState->tables.lockExclusive();
          asyncOp = kj::mv(lock->answers[questionId].asyncOp);
        }

        // Delete the promise, thereby canceling the operation.  Note that if a continuation is
        // running in another thread, this line blocks waiting for it to complete.  This is why
        // we try to schedule doCancel() on the application thread, so that it won't need to block.
        asyncOp = nullptr;

        // OK, now that we know the call isn't running in another thread, we can drop our thread
        // safety and send a return message.
        const_cast<RpcCallContext*>(this)->sendCancel();
      });
    }

    bool isFirstResponder() {
      // The first time it is called, removes self from the answer table and returns true.
      // On subsequent calls, returns false.

      kj::Own<const PipelineHook> pipelineToRelease;

      if (responseSent) {
        return false;
      } else {
        responseSent = true;

        // We need to remove the `callContext` pointer -- which points back to us -- from the
        // answer table.  Or we might even be responsible for removing the entire answer table
        // entry.
        auto lock = connectionState->tables.lockExclusive();

        if (__atomic_load_n(&cancellationFlags, __ATOMIC_RELAXED) & CANCEL_REQUESTED) {
          // We are responsible for deleting the answer table entry.  Awkwardly, however, the
          // answer table may be the only thing holding a reference to the context, and we may even
          // be called from the continuation represented by answer.asyncOp.  So we have to do the
          // actual deletion asynchronously.  But we have to remove it from the table *now*, while
          // we still hold the lock, because once we send the return message the answer ID is free
          // for reuse.
          auto promise = connectionState->eventLoop.evalLater([]() {});
          promise.attach(kj::mv(lock->answers[questionId]));
          connectionState->tasks.add(kj::mv(promise));

          // Erase from the table.
          lock->answers.erase(questionId);
        } else {
          // We just have to null out callContext.
          auto& answer = lock->answers[questionId];
          answer.callContext = nullptr;

          // If the response has no capabilities in it, then we should also delete the pipeline
          // so that the context can be released sooner.
          KJ_IF_MAYBE(r, response) {
            if (!r->get()->hasCaps()) {
              KJ_IF_MAYBE(pipeline, answer.pipeline) {
                pipelineToRelease = kj::mv(*pipeline);
              }
            }
          }
        }

        return true;
      }
    }
  };

  // =====================================================================================
  // Message handling

  kj::Promise<void> messageLoop() {
    auto receive = eventLoop.there(connection->receiveIncomingMessage(),
        [this](kj::Maybe<kj::Own<IncomingRpcMessage>>&& message) {
          KJ_IF_MAYBE(m, message) {
            handleMessage(kj::mv(*m));
          } else {
            KJ_FAIL_REQUIRE("Peer disconnected.") { break; }
          }
        });
    return eventLoop.there(kj::mv(receive),
        [this]() {
          // No exceptions; continue loop.
          //
          // (We do this in a separate continuation to handle the case where exceptions are
          // disabled.)
          tasks.add(messageLoop());
        });
  }

  void handleMessage(kj::Own<IncomingRpcMessage> message) {
    auto reader = message->getBody().getAs<rpc::Message>();
    switch (reader.which()) {
      case rpc::Message::UNIMPLEMENTED:
        handleUnimplemented(reader.getUnimplemented());
        break;

      case rpc::Message::ABORT:
        handleAbort(reader.getAbort());
        break;

      case rpc::Message::CALL:
        handleCall(kj::mv(message), reader.getCall());
        break;

      case rpc::Message::RETURN:
        handleReturn(kj::mv(message), reader.getReturn());
        break;

      case rpc::Message::FINISH:
        handleFinish(reader.getFinish());
        break;

      case rpc::Message::RESOLVE:
        // TODO(now)
        break;

      case rpc::Message::RELEASE:
        // TODO(now)
        break;

      case rpc::Message::RESTORE:
        handleRestore(kj::mv(message), reader.getRestore());
        break;

      default: {
        auto message = connection->newOutgoingMessage(
            reader.totalSizeInWords() + messageSizeHint<void>());
        message->getBody().initAs<rpc::Message>().setUnimplemented(reader);
        message->send();
        break;
      }
    }
  }

  void handleUnimplemented(const rpc::Message::Reader& message) {
    switch (message.which()) {
      case rpc::Message::RESOLVE:
        // TODO(now):  Release the resolution.
        break;

      default:
        KJ_FAIL_ASSERT("Peer did not implement required RPC message type.", (uint)message.which());
        break;
    }
  }

  void handleAbort(const rpc::Exception::Reader& exception) {
    kj::throwRecoverableException(toException(exception));
  }

  // ---------------------------------------------------------------------------
  // Level 0

  void handleCall(kj::Own<IncomingRpcMessage>&& message, const rpc::Call::Reader& call) {
    kj::Own<const ClientHook> capability;

    auto target = call.getTarget();
    switch (target.which()) {
      case rpc::Call::Target::EXPORTED_CAP: {
        auto lock = tables.lockExclusive();  // TODO(perf):  shared?
        KJ_IF_MAYBE(exp, lock->exports.find(target.getExportedCap())) {
          capability = exp->clientHook->addRef();
        } else {
          KJ_FAIL_REQUIRE("Call target is not a current export ID.") {
            return;
          }
        }
        break;
      }

      case rpc::Call::Target::PROMISED_ANSWER: {
        auto promisedAnswer = target.getPromisedAnswer();
        kj::Own<const PipelineHook> pipeline;

        {
          auto lock = tables.lockExclusive();  // TODO(perf):  shared?
          auto& base = lock->answers[promisedAnswer.getQuestionId()];
          KJ_REQUIRE(base.active, "PromisedAnswer.questionId is not a current question.") {
            return;
          }
          KJ_IF_MAYBE(p, base.pipeline) {
            pipeline = p->get()->addRef();
          } else {
            KJ_FAIL_REQUIRE("PromisedAnswer.questionId is already finished or contained no "
                            "capabilities.") {
              return;
            }
          }
        }

        KJ_IF_MAYBE(ops, toPipelineOps(promisedAnswer.getTransform())) {
          capability = pipeline->getPipelinedCap(*ops);
        } else {
          // Exception already thrown.
          return;
        }
        break;
      }

      default:
        // TODO(soon):  Handle better.
        KJ_FAIL_REQUIRE("Unknown call target type.", (uint)target.which()) {
          return;
        }
    }

    QuestionId questionId = call.getQuestionId();
    auto context = kj::refcounted<RpcCallContext>(
        *this, questionId, kj::mv(message), call.getParams());
    auto promiseAndPipeline = capability->call(
        call.getInterfaceId(), call.getMethodId(), context->addRef());

    // No more using `call` after this point!

    {
      auto lock = tables.lockExclusive();

      auto& answer = lock->answers[questionId];

      // We don't want to overwrite an active question because the destructors for the promise and
      // pipeline could try to lock our mutex.  Of course, we did already fire off the new call
      // above, but that's OK because it won't actually ever inspect the Answer table itself, and
      // we're about to close the connection anyway.
      KJ_REQUIRE(!answer.active, "questionId is already in use") {
        return;
      }

      answer.active = true;
      answer.callContext = *context;
      answer.pipeline = kj::mv(promiseAndPipeline.pipeline);

      // Hack:  Both the success and error continuations need to use the context.  We could
      //   refcount, but both will be destroyed at the same time anyway.
      RpcCallContext* contextPtr = context;

      answer.asyncOp = promiseAndPipeline.promise.then(
          [contextPtr]() {
            contextPtr->sendReturn();
          }, [contextPtr](kj::Exception&& exception) {
            contextPtr->sendErrorReturn(kj::mv(exception));
          });
      answer.asyncOp.attach(kj::mv(context));
      answer.asyncOp.eagerlyEvaluate(eventLoop);
    }
  }

  void handleReturn(kj::Own<IncomingRpcMessage>&& message, const rpc::Return::Reader& ret) {
    kj::Own<CapInjectorImpl> paramCapsToRelease;

    auto lock = tables.lockExclusive();
    KJ_IF_MAYBE(question, lock->questions.find(ret.getQuestionId())) {
      KJ_REQUIRE(question->paramCaps != nullptr, "Duplicate Return.") { return; }

      KJ_IF_MAYBE(pc, question->paramCaps) {
        // Release these later, after unlocking.
        paramCapsToRelease = kj::mv(*pc);
      } else {
        KJ_FAIL_REQUIRE("Duplicate return.") { return; }
      }

      for (ExportId retained: ret.getRetainedCaps()) {
        KJ_IF_MAYBE(exp, lock->exports.find(retained)) {
          ++exp->refcount;
        } else {
          KJ_FAIL_REQUIRE("Invalid export ID in Return.retainedCaps list.") { return; }
        }
      }

      switch (ret.which()) {
        case rpc::Return::RESULTS:
          KJ_IF_MAYBE(questionRef, question->selfRef) {
            // The questionRef still exists, but could be being deleted in another thread.
            KJ_IF_MAYBE(ownRef, kj::tryAddRef(*questionRef)) {
              // Not being deleted.
              questionRef->fulfill(kj::refcounted<RpcResponse>(
                  *this, kj::mv(*ownRef), kj::mv(message), ret.getResults()));
            }
          }
          break;

        case rpc::Return::EXCEPTION:
          KJ_IF_MAYBE(questionRef, question->selfRef) {
            // The questionRef still exists, but could be being deleted in another thread.
            KJ_IF_MAYBE(ownRef, kj::tryAddRef(*questionRef)) {
              questionRef->reject(toException(ret.getException()));
            }
          }
          break;

        case rpc::Return::CANCELED:
          KJ_REQUIRE(question->selfRef == nullptr,
                     "Return message falsely claims call was canceled.") { return; }
          // We don't bother fulfilling the result.  If someone is somehow still waiting on it
          // (shouldn't be possible), that's OK:  they'll get an exception due to the fulfiller
          // being destroyed.
          break;

        default:
          KJ_FAIL_REQUIRE("Unknown return type (not answer, exception, or canceled).") { return; }
      }

      if (question->selfRef == nullptr) {
        lock->questions.erase(ret.getQuestionId());
      }

    } else {
      KJ_FAIL_REQUIRE("Invalid question ID in Return message.") { return; }
    }
  }

  void handleFinish(const rpc::Finish::Reader& finish) {
    kj::Maybe<kj::Own<const PipelineHook>> pipelineToRelease;

    auto lock = tables.lockExclusive();

    for (ExportId retained: finish.getRetainedCaps()) {
      KJ_IF_MAYBE(exp, lock->exports.find(retained)) {
        ++exp->refcount;
      } else {
        KJ_FAIL_REQUIRE("Invalid export ID in Return.retainedCaps list.") { return; }
      }
    }

    auto& answer = lock->answers[finish.getQuestionId()];

    // `Finish` indicates that no further pipeline requests will be made.
    pipelineToRelease = kj::mv(answer.pipeline);

    KJ_IF_MAYBE(context, answer.callContext) {
      context->requestCancel();
    } else {
      lock->answers.erase(finish.getQuestionId());
    }
  }

  // ---------------------------------------------------------------------------
  // Level 1

  // ---------------------------------------------------------------------------
  // Level 2

  class SingleCapPipeline: public PipelineHook, public kj::Refcounted {
  public:
    SingleCapPipeline(kj::Own<const ClientHook>&& cap,
                      kj::Own<CapInjectorImpl>&& capInjector)
        : cap(kj::mv(cap)), capInjector(kj::mv(capInjector)) {}

    kj::Own<const PipelineHook> addRef() const override {
      return kj::addRef(*this);
    }

    kj::Own<const ClientHook> getPipelinedCap(kj::ArrayPtr<const PipelineOp> ops) const override {
      if (ops.size() == 0) {
        return cap->addRef();
      } else {
        return newBrokenCap("Invalid pipeline transform.");
      }
    }

  private:
    kj::Own<const ClientHook> cap;
    kj::Own<CapInjectorImpl> capInjector;
  };

  void handleRestore(kj::Own<IncomingRpcMessage>&& message, const rpc::Restore::Reader& restore) {
    QuestionId questionId = restore.getQuestionId();

    auto response = connection->newOutgoingMessage(
        messageSizeHint<rpc::Return>() + sizeInWords<rpc::CapDescriptor>() + 32);

    rpc::Return::Builder ret = response->getBody().getAs<rpc::Message>().initReturn();
    ret.setQuestionId(questionId);

    auto injector = kj::heap<CapInjectorImpl>(*this);
    CapBuilderContext context(*injector);

    kj::Own<const ClientHook> capHook;

    // Call the restorer and initialize the answer.
    KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
      KJ_IF_MAYBE(r, restorer) {
        Capability::Client cap = r->baseRestore(restore.getObjectId());
        auto results = context.imbue(ret.initResults());
        results.setAs<Capability>(cap);

        // Hack to extract the ClientHook, because Capability::Client doesn't provide direct
        // access.  Maybe it should?
        capHook = results.asReader().getPipelinedCap(nullptr);
      } else {
        KJ_FAIL_REQUIRE("This vat cannot restore this SturdyRef.") { break; }
      }
    })) {
      fromException(*exception, ret.initException());
      capHook = newBrokenCap(kj::mv(*exception));
    }

    message = nullptr;

    // Add the answer to the answer table for pipelining and send the response.
    {
      auto lock = tables.lockExclusive();

      auto& answer = lock->answers[questionId];
      KJ_REQUIRE(!answer.active, "questionId is already in use") {
        return;
      }

      injector->finishDescriptors(*lock);

      answer.active = true;
      answer.pipeline = kj::Own<const PipelineHook>(
          kj::refcounted<SingleCapPipeline>(kj::mv(capHook), kj::mv(injector)));

      response->send();
    }
  }

  // =====================================================================================

  void sendReleaseLater(ExportId importId, uint remoteRefcount) const {
    tasks.add(eventLoop.evalLater([this,importId,remoteRefcount]() {
      auto message = connection->newOutgoingMessage(messageSizeHint<rpc::Release>());
      rpc::Release::Builder builder = message->getBody().initAs<rpc::Message>().initRelease();
      builder.setId(importId);
      builder.setReferenceCount(remoteRefcount);
      message->send();
    }));
  }
};

}  // namespace

class RpcSystemBase::Impl final: public kj::TaskSet::ErrorHandler {
public:
  Impl(VatNetworkBase& network, kj::Maybe<SturdyRefRestorerBase&> restorer,
       const kj::EventLoop& eventLoop)
      : network(network), restorer(restorer), eventLoop(eventLoop), tasks(eventLoop, *this) {
    tasks.add(acceptLoop());
  }

  ~Impl() noexcept(false) {
    // std::unordered_map doesn't like it when elements' destructors throw, so carefully
    // disassemble it.
    auto& connectionMap = connections.getWithoutLock();
    if (!connectionMap.empty()) {
      kj::Vector<kj::Own<RpcConnectionState>> deleteMe(connectionMap.size());
      kj::Exception shutdownException(
          kj::Exception::Nature::LOCAL_BUG, kj::Exception::Durability::PERMANENT,
          __FILE__, __LINE__, kj::str("RpcSystem was destroyed."));
      for (auto& entry: connectionMap) {
        entry.second->taskFailed(kj::cp(shutdownException));
        deleteMe.add(kj::mv(entry.second));
      }
    }
  }

  Capability::Client restore(_::StructReader hostId, ObjectPointer::Reader objectId) {
    KJ_IF_MAYBE(connection, network.baseConnectToRefHost(hostId)) {
      auto lock = connections.lockExclusive();
      auto& state = getConnectionState(kj::mv(*connection), *lock);
      return Capability::Client(state.restore(objectId));
    } else KJ_IF_MAYBE(r, restorer) {
      return r->baseRestore(objectId);
    } else {
      return Capability::Client(newBrokenCap(
          "SturdyRef referred to a local object but there is no local SturdyRef restorer."));
    }
  }

  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, exception);
  }

private:
  VatNetworkBase& network;
  kj::Maybe<SturdyRefRestorerBase&> restorer;
  const kj::EventLoop& eventLoop;
  kj::TaskSet tasks;

  typedef std::unordered_map<VatNetworkBase::Connection*, kj::Own<RpcConnectionState>>
      ConnectionMap;
  kj::MutexGuarded<ConnectionMap> connections;

  RpcConnectionState& getConnectionState(kj::Own<VatNetworkBase::Connection>&& connection,
                                         ConnectionMap& lockedMap) {
    auto iter = lockedMap.find(connection);
    if (iter == lockedMap.end()) {
      VatNetworkBase::Connection* connectionPtr = connection;
      auto onDisconnect = kj::newPromiseAndFulfiller<void>();
      tasks.add(eventLoop.there(kj::mv(onDisconnect.promise), [this,connectionPtr]() {
        connections.lockExclusive()->erase(connectionPtr);
      }));
      auto newState = kj::refcounted<RpcConnectionState>(
          eventLoop, restorer, kj::mv(connection), kj::mv(onDisconnect.fulfiller));
      RpcConnectionState& result = *newState;
      lockedMap.insert(std::make_pair(connectionPtr, kj::mv(newState)));
      return result;
    } else {
      return *iter->second;
    }
  }

  kj::Promise<void> acceptLoop() {
    auto receive = eventLoop.there(network.baseAcceptConnectionAsRefHost(),
        [this](kj::Own<VatNetworkBase::Connection>&& connection) {
          auto lock = connections.lockExclusive();
          getConnectionState(kj::mv(connection), *lock);
        });
    return eventLoop.there(kj::mv(receive),
        [this]() {
          // No exceptions; continue loop.
          //
          // (We do this in a separate continuation to handle the case where exceptions are
          // disabled.)
          tasks.add(acceptLoop());
        });
  }
};

RpcSystemBase::RpcSystemBase(VatNetworkBase& network, kj::Maybe<SturdyRefRestorerBase&> restorer,
                             const kj::EventLoop& eventLoop)
    : impl(kj::heap<Impl>(network, restorer, eventLoop)) {}
RpcSystemBase::RpcSystemBase(RpcSystemBase&& other) noexcept = default;
RpcSystemBase::~RpcSystemBase() noexcept(false) {}

Capability::Client RpcSystemBase::baseRestore(
    _::StructReader hostId, ObjectPointer::Reader objectId) {
  return impl->restore(hostId, objectId);
}

}  // namespace _ (private)
}  // namespace capnp
