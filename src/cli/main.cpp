#include <iostream>
#include <vector>
#include "cli/prepare_playback.h"
#include "cli/arguments.h"
#include "engine/hardware_pipeline_factory.h"
#include "engine/session_controller.h"

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && std::wstring_view(argv[1]) == L"--prepare-playback")
    return prepare_playback(argv[2], argv[3]);
  using namespace rebelliocap;
  std::vector<std::wstring_view> values;
  for(int i=1;i<argc;++i) values.emplace_back(argv[i]);
  auto parsed=parse_arguments(values);
  if(!parsed.is_success()) {
    std::cerr<<parsed.error().code<<": "<<parsed.error().message<<'\n';
    return static_cast<int>(CliExitCode::InvalidArguments);
  }
  const auto& arguments=parsed.value();
  if(arguments.command==CliCommand::DoctorJson || arguments.command==CliCommand::CatalogJson ||
      arguments.command==CliCommand::TestJson || arguments.command==CliCommand::SessionJson)
    return static_cast<int>(run_native_host(arguments,std::cout));
  DiagnosticWriter writer(std::cout);
  HardwarePipelineFactory factory;
  return static_cast<int>(factory.run_legacy(arguments,writer));
}
