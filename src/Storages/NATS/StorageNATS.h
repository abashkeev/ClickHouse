#pragma once

#include <atomic>
#include <mutex>
#include <uv.h>
#include <Core/BackgroundSchedulePoolTaskHolder.h>
#include <Core/StreamingHandleErrorMode.h>
#include <Storages/IStorage.h>
#include <Storages/NATS/NATSConnection.h>
#include <Storages/NATS/NATSHandler.h>
#include <Storages/NATS/NATSSettings.h>
#include <Storages/NATS/NATS_fwd.h>
#include <Poco/Semaphore.h>
#include <Common/thread_local_rng.h>

namespace DB
{

class NATSConsumer;
using NATSConsumerPtr = std::shared_ptr<NATSConsumer>;
struct NATSSettings;

class StorageNATS final : public IStorage, WithContext
{
public:
    StorageNATS(
        const StorageID & table_id_,
        ContextPtr context_,
        const ColumnsDescription & columns_,
        const String & comment,
        std::unique_ptr<NATSSettings> nats_settings_,
        LoadingStrictnessLevel mode);

    ~StorageNATS() override;

    std::string getName() const override { return NATS::TABLE_ENGINE_NAME; }

    bool noPushingToViewsOnInserts() const override { return true; }

    void startup() override;
    void shutdown(bool is_drop) override;

    /// This is a bad way to let storage know in shutdown() that table is going to be dropped. There are some actions which need
    /// to be done only when table is dropped (not when detached). Also connection must be closed only in shutdown, but those
    /// actions require an open connection. Therefore there needs to be a way inside shutdown() method to know whether it is called
    /// because of drop query. And drop() method is not suitable at all, because it will not only require to reopen connection, but also
    /// it can be called considerable time after table is dropped (for example, in case of Atomic database), which is not appropriate for the case.
    void checkTableCanBeDropped([[ maybe_unused ]] ContextPtr query_context) const override { drop_table = true; }

    /// Always return virtual columns in addition to required columns
    void read(
        QueryPlan & query_plan,
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr local_context,
        QueryProcessingStage::Enum /* processed_stage */,
        size_t /* max_block_size */,
        size_t /* num_streams */) override;

    SinkToStoragePtr write(const ASTPtr & query, const StorageMetadataPtr & metadata_snapshot, ContextPtr context, bool async_insert) override;

    /// We want to control the number of rows in a chunk inserted into NATS
    bool prefersLargeBlocks() const override { return false; }

    void pushConsumer(NATSConsumerPtr consumer);
    NATSConsumerPtr popConsumer();
    NATSConsumerPtr popConsumer(std::chrono::milliseconds timeout);

    const String & getFormatName() const { return format_name; }

private:
    ContextMutablePtr nats_context;
    std::unique_ptr<NATSSettings> nats_settings;
    std::vector<String> subjects;

    const String format_name;
    const String schema_name;
    size_t num_consumers;
    size_t max_rows_per_message;

    LoggerPtr log;

    NATSHandler event_handler;
    std::unique_ptr<ThreadFromGlobalPool> event_loop_thread;

    NATSConnectionPtr consumers_connection; /// Connection for all consumers
    NATSConfiguration configuration;

    std::atomic<size_t> num_created_consumers = 0;
    Poco::Semaphore semaphore;
    std::mutex consumers_mutex;
    std::vector<NATSConsumerPtr> consumers; /// available NATS consumers

    /// maximum number of messages in NATS queue (x-max-length). Also used
    /// to setup size of inner consumer for received messages
    uint32_t queue_size;

    std::mutex task_mutex;
    BackgroundSchedulePoolTaskHolder streaming_task;
    BackgroundSchedulePoolTaskHolder initialize_consumers_task;

    /// True if consumers have subscribed to all subjects
    std::atomic<bool> consumers_ready{false};
    /// Needed for tell MV or producer background tasks
    /// that they must finish as soon as possible.
    std::atomic<bool> shutdown_called{false};
    std::atomic<bool> mv_attached = false;

    mutable bool drop_table = false;
    bool throw_on_startup_failure;

    NATSConsumerPtr createConsumer();

    bool isSubjectInSubscriptions(const std::string & subject);

    /// Functions working in the background
    void initializeConsumersFunc();
    void streamingToViewsFunc();

    void createConsumersConnection();
    void createConsumers();

    bool subscribeConsumers();
    void unsubscribeConsumers();

    void stopEventLoop();

    static Names parseList(const String & list, char delim);
    static String getTableBasedName(String name, const StorageID & table_id);
    static VirtualColumnsDescription createVirtuals(StreamingHandleErrorMode handle_error_mode);

    ContextMutablePtr addSettings(ContextPtr context) const;
    size_t getMaxBlockSize() const;
    void deactivateTask(BackgroundSchedulePoolTaskHolder & task);

    bool streamToViews();
    bool checkDependencies(const StorageID & table_id);
};

}
