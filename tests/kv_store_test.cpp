#include <gtest/gtest.h>
#include <msgpack.hpp>
#include <server/kv_store.h>

using namespace kv;

// Test msgpack serialization and deserialization of Command struct
TEST(KVStoreTest, CommandSerializationSet) {
  // Create a Set command
  Command cmd;
  cmd.type = CommandType::Set;
  cmd.strs = {"key1", "value1"};

  // Serialize to msgpack
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, cmd);

  // Deserialize back
  msgpack::object_handle oh =
      msgpack::unpack(sbuf.data(), sbuf.size());
  msgpack::object obj = oh.get();

  Command result;
  obj.convert(result);

  // Verify
  EXPECT_EQ(result.type, CommandType::Set);
  EXPECT_EQ(result.strs.size(), 2);
  EXPECT_EQ(result.strs[0], "key1");
  EXPECT_EQ(result.strs[1], "value1");
}

TEST(KVStoreTest, CommandSerializationDel) {
  // Create a Del command with multiple keys
  Command cmd;
  cmd.type = CommandType::Del;
  cmd.strs = {"key1", "key2", "key3"};

  // Serialize to msgpack
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, cmd);

  // Deserialize back
  msgpack::object_handle oh =
      msgpack::unpack(sbuf.data(), sbuf.size());
  msgpack::object obj = oh.get();

  Command result;
  obj.convert(result);

  // Verify
  EXPECT_EQ(result.type, CommandType::Del);
  EXPECT_EQ(result.strs.size(), 3);
  EXPECT_EQ(result.strs[0], "key1");
  EXPECT_EQ(result.strs[1], "key2");
  EXPECT_EQ(result.strs[2], "key3");
}

// Test basic KVStore operations
TEST(KVStoreTest, BasicSetAndGet) {
  KVStore store;
  std::string value;

  // Set a key
  store.set("name", "Alice");

  // Get the key
  bool found = store.get("name", value);
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "Alice");
}

TEST(KVStoreTest, GetNonExistentKey) {
  KVStore store;
  std::string value;

  // Try to get a key that doesn't exist
  bool found = store.get("missing", value);
  EXPECT_FALSE(found);
}

TEST(KVStoreTest, DeleteKey) {
  KVStore store;
  std::string value;

  // Set a key
  store.set("temp", "data");

  // Verify it exists
  EXPECT_TRUE(store.get("temp", value));

  // Delete it
  store.del("temp");

  // Verify it's gone
  EXPECT_FALSE(store.get("temp", value));
}

TEST(KVStoreTest, OverwriteExistingKey) {
  KVStore store;
  std::string value;

  // Set a key
  store.set("counter", "1");

  // Overwrite it
  store.set("counter", "2");

  // Verify new value
  bool found = store.get("counter", value);
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "2");
}

TEST(KVStoreTest, InvalidSnapshotDoesNotReplaceCurrentState) {
  KVStore store;
  store.set("stable", "value");

  EXPECT_THROW(store.deserialize({0xc1}), std::exception);

  std::string value;
  ASSERT_TRUE(store.get("stable", value));
  EXPECT_EQ(value, "value");
}

// Test apply() method with Raft integration
TEST(KVStoreTest, ApplySetCommand) {
  KVStore store;
  std::string value;

  // Create a Set command
  Command cmd;
  cmd.type = CommandType::Set;
  cmd.strs = {"user", "Bob"};

  // Serialize to msgpack
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, cmd);

  // Create a Raft entry
  proto::Entry entry;
  entry.type = proto::EntryNormal;
  entry.term = 1;
  entry.index = 1;
  entry.data.assign(sbuf.data(), sbuf.data() + sbuf.size());

  // Apply the entry
  store.apply(entry);

  // Verify the command was applied
  bool found = store.get("user", value);
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "Bob");
}

TEST(KVStoreTest, ApplyDelCommand) {
  KVStore store;
  std::string value;

  // First, set some keys directly
  store.set("key1", "value1");
  store.set("key2", "value2");
  store.set("key3", "value3");

  // Create a Del command
  Command cmd;
  cmd.type = CommandType::Del;
  cmd.strs = {"key1", "key3"};

  // Serialize to msgpack
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, cmd);

  // Create a Raft entry
  proto::Entry entry;
  entry.type = proto::EntryNormal;
  entry.term = 1;
  entry.index = 2;
  entry.data.assign(sbuf.data(), sbuf.data() + sbuf.size());

  // Apply the entry
  store.apply(entry);

  // Verify key1 and key3 are deleted
  EXPECT_FALSE(store.get("key1", value));
  EXPECT_FALSE(store.get("key3", value));

  // Verify key2 still exists
  EXPECT_TRUE(store.get("key2", value));
  EXPECT_EQ(value, "value2");
}

TEST(KVStoreTest, MultipleOperations) {
  KVStore store;
  std::string value;

  // Apply multiple Set commands through Raft
  for (int i = 0; i < 5; i++) {
    Command cmd;
    cmd.type = CommandType::Set;
    cmd.strs = {"key" + std::to_string(i), "value" + std::to_string(i)};

    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, cmd);

    proto::Entry entry;
    entry.type = proto::EntryNormal;
    entry.term = 1;
    entry.index = i + 1;
    entry.data.assign(sbuf.data(), sbuf.data() + sbuf.size());

    store.apply(entry);
  }

  // Verify all keys exist
  for (int i = 0; i < 5; i++) {
    bool found = store.get("key" + std::to_string(i), value);
    EXPECT_TRUE(found);
    EXPECT_EQ(value, "value" + std::to_string(i));
  }
}

TEST(KVStoreTest, ApplyReturnsRequestIdentityAndDeleteCount) {
  KVStore store;
  store.set("present", "value");

  Command command;
  command.type = CommandType::Del;
  command.strs = {"present", "missing"};
  command.origin_node = 2;
  command.request_id = 99;

  msgpack::sbuffer buffer;
  msgpack::pack(buffer, command);
  proto::Entry entry;
  entry.index = 7;
  entry.term = 3;
  entry.data.assign(buffer.data(), buffer.data() + buffer.size());

  const auto result = store.apply(entry);

  EXPECT_EQ(result.origin_node, 2u);
  EXPECT_EQ(result.request_id, 99u);
  EXPECT_EQ(result.affected, 1u);
  EXPECT_EQ(result.applied_index, 7u);
}

TEST(KVStoreTest, AppliesLegacyTwoFieldCommandFromExistingWal) {
  msgpack::sbuffer buffer;
  msgpack::packer<msgpack::sbuffer> packer(buffer);
  packer.pack_array(2);
  packer.pack(static_cast<uint8_t>(CommandType::Set));
  packer.pack(std::vector<std::string>{"legacy", "value"});

  proto::Entry entry;
  entry.index = 1;
  entry.term = 1;
  entry.data.assign(buffer.data(), buffer.data() + buffer.size());

  KVStore store;
  const auto result = store.apply(entry);

  std::string value;
  ASSERT_TRUE(store.get("legacy", value));
  EXPECT_EQ(value, "value");
  EXPECT_EQ(result.origin_node, 0u);
  EXPECT_EQ(result.request_id, 0u);
}
