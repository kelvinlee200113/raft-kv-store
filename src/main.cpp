#include <app/node_runtime.h>

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << kv::app::node_usage(argv[0]);
    return 0;
  }

  try {
    kv::app::NodeRuntime node(kv::app::parse_node_config(argc, argv));
    return node.run();
  } catch (const std::exception &error) {
    std::cerr << "raft-kv: " << error.what() << "\n\n"
              << kv::app::node_usage(argv[0]);
    return 1;
  }
}
