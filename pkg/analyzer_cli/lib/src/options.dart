// Copyright (c) 2015, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library analyzer_cli.src.options;

import 'dart:io';

import 'package:analyzer/file_system/physical_file_system.dart';
import 'package:analyzer/src/command_line/arguments.dart';
import 'package:analyzer/src/context/builder.dart';
import 'package:analyzer_cli/src/driver.dart';
import 'package:args/args.dart';
import 'package:cli_util/cli_util.dart' show getSdkDir;

const _binaryName = 'dartanalyzer';

/// Shared exit handler.
///
/// *Visible for testing.*
ExitHandler exitHandler = exit;

/// Print the given [message] to stderr and exit with the given [exitCode].
void printAndFail(String message, {int exitCode: 15}) {
  errorSink.writeln(message);
  exitHandler(exitCode);
}

/// Exit handler.
///
/// *Visible for testing.*
typedef void ExitHandler(int code);

/// Analyzer commandline configuration options.
class CommandLineOptions {
  /// The path to output analysis results when in build mode.
  final String buildAnalysisOutput;

  /// Whether to use build mode.
  final bool buildMode;

  /// Whether to use build mode as a Bazel persistent worker.
  final bool buildModePersistentWorker;

  /// List of summary file paths to use in build mode.
  final List<String> buildSummaryInputs;

  /// Whether to skip analysis when creating summaries in build mode.
  final bool buildSummaryOnly;

  /// Whether to use diet parsing, i.e. skip function bodies. We don't need to
  /// analyze function bodies to use summaries during future compilation steps.
  final bool buildSummaryOnlyDiet;

  /// Whether to use exclude informative data from created summaries.
  final bool buildSummaryExcludeInformative;

  /// The path to output the summary when creating summaries in build mode.
  final String buildSummaryOutput;

  /// The path to output the semantic-only summary when creating summaries in
  /// build mode.
  final String buildSummaryOutputSemantic;

  /// Whether to suppress a nonzero exit code in build mode.
  final bool buildSuppressExitCode;

  /// The options defining the context in which analysis is performed.
  final ContextBuilderOptions contextBuilderOptions;

  /// The path to the dart SDK.
  String dartSdkPath;

  /// The path to the dart SDK summary file.
  String dartSdkSummaryPath;

  /// Whether to disable cache flushing.  This option can improve analysis
  /// speed at the expense of memory usage.  It may also be useful for working
  /// around bugs.
  final bool disableCacheFlushing;

  /// Whether to report hints
  final bool disableHints;

  /// Whether to display version information
  final bool displayVersion;

  /// Whether to enable null-aware operators (DEP 9).
  final bool enableNullAwareOperators;

  /// Whether to treat type mismatches found during constant evaluation as
  /// errors.
  final bool enableTypeChecks;

  /// Whether to treat hints as fatal
  final bool hintsAreFatal;

  /// Whether to ignore unrecognized flags
  final bool ignoreUnrecognizedFlags;

  /// Whether to report lints
  final bool lints;

  /// Whether to log additional analysis messages and exceptions
  final bool log;

  /// Whether to use machine format for error display
  final bool machineFormat;

  /// The path to a file to write a performance log.
  /// (Or null if not enabled.)
  final String perfReport;

  /// Batch mode (for unit testing)
  final bool shouldBatch;

  /// Whether to show package: warnings
  final bool showPackageWarnings;

  /// If not null, show package: warnings only for matching packages.
  final String showPackageWarningsPrefix;

  /// Whether to show SDK warnings
  final bool showSdkWarnings;

  /// The source files to analyze
  final List<String> sourceFiles;

  /// Whether to treat warnings as fatal
  final bool warningsAreFatal;

  /// Whether to use strong static checking.
  final bool strongMode;

  /// Whether implicit casts are enabled (in strong mode)
  final bool implicitCasts;

  /// Whether implicit dynamic is enabled (mainly for strong mode users)
  final bool implicitDynamic;

  /// Whether to treat lints as fatal
  final bool lintsAreFatal;

  /// Initialize options from the given parsed [args].
  CommandLineOptions._fromArgs(ArgResults args)
      : buildAnalysisOutput = args['build-analysis-output'],
        buildMode = args['build-mode'],
        buildModePersistentWorker = args['persistent_worker'],
        buildSummaryInputs = args['build-summary-input'] as List<String>,
        buildSummaryOnly = args['build-summary-only'],
        buildSummaryOnlyDiet = args['build-summary-only-diet'],
        buildSummaryExcludeInformative =
            args['build-summary-exclude-informative'],
        buildSummaryOutput = args['build-summary-output'],
        buildSummaryOutputSemantic = args['build-summary-output-semantic'],
        buildSuppressExitCode = args['build-suppress-exit-code'],
        contextBuilderOptions = createContextBuilderOptions(args),
        dartSdkPath = args['dart-sdk'],
        dartSdkSummaryPath = args['dart-sdk-summary'],
        disableCacheFlushing = args['disable-cache-flushing'],
        disableHints = args['no-hints'],
        displayVersion = args['version'],
        enableNullAwareOperators = args['enable-null-aware-operators'],
        enableTypeChecks = args['enable_type_checks'],
        hintsAreFatal = args['fatal-hints'],
        ignoreUnrecognizedFlags = args['ignore-unrecognized-flags'],
        lints = args['lints'],
        log = args['log'],
        machineFormat = args['machine'] || args['format'] == 'machine',
        perfReport = args['x-perf-report'],
        shouldBatch = args['batch'],
        showPackageWarnings = args['show-package-warnings'] ||
            args['package-warnings'] ||
            args['x-package-warnings-prefix'] != null,
        showPackageWarningsPrefix = args['x-package-warnings-prefix'],
        showSdkWarnings = args['show-sdk-warnings'] || args['warnings'],
        sourceFiles = args.rest,
        warningsAreFatal = args['fatal-warnings'],
        strongMode = args['strong'],
        implicitCasts = !args['no-implicit-casts'],
        implicitDynamic = !args['no-implicit-dynamic'],
        lintsAreFatal = args['fatal-lints'];

  /// The path to an analysis options file
  String get analysisOptionsFile =>
      contextBuilderOptions.defaultAnalysisOptionsFilePath;

  /// A table mapping the names of defined variables to their values.
  Map<String, String> get definedVariables =>
      contextBuilderOptions.declaredVariables;

  /// Whether to strictly follow the specification when generating warnings on
  /// "call" methods (fixes dartbug.com/21938).
  bool get enableStrictCallChecks =>
      contextBuilderOptions.defaultOptions.enableStrictCallChecks;

  /// Whether to relax restrictions on mixins (DEP 34).
  bool get enableSuperMixins =>
      contextBuilderOptions.defaultOptions.enableSuperMixins;

  /// The path to the package root
  String get packageRootPath =>
      contextBuilderOptions.defaultPackagesDirectoryPath;

  /// The path to a `.packages` configuration file
  String get packageConfigPath => contextBuilderOptions.defaultPackageFilePath;

  /// Parse [args] into [CommandLineOptions] describing the specified
  /// analyzer options. In case of a format error, calls [printAndFail], which
  /// by default prints an error message to stderr and exits.
  static CommandLineOptions parse(List<String> args,
      [printAndFail(String msg) = printAndFail]) {
    CommandLineOptions options = _parse(args);
    // Check SDK.
    if (!options.buildModePersistentWorker) {
      // Infer if unspecified.
      if (options.dartSdkPath == null) {
        Directory sdkDir = getSdkDir(args);
        if (sdkDir != null) {
          options.dartSdkPath = sdkDir.path;
        }
      }

      var sdkPath = options.dartSdkPath;

      // Check that SDK is specified.
      if (sdkPath == null) {
        printAndFail('No Dart SDK found.');
        return null; // Only reachable in testing.
      }
      // Check that SDK is existing directory.
      if (!(new Directory(sdkPath)).existsSync()) {
        printAndFail('Invalid Dart SDK path: $sdkPath');
        return null; // Only reachable in testing.
      }
    }

    // Check package config.
    {
      if (options.packageRootPath != null &&
          options.packageConfigPath != null) {
        printAndFail("Cannot specify both '--package-root' and '--packages.");
        return null; // Only reachable in testing.
      }
    }

    // OK.  Report deprecated options.
    if (options.enableNullAwareOperators) {
      errorSink.writeln(
          "Info: Option '--enable-null-aware-operators' is no longer needed. "
          "Null aware operators are supported by default.");
    }

    // Build mode.
    if (options.buildModePersistentWorker && !options.buildMode) {
      printAndFail('The option --persisten_worker can be used only '
          'together with --build-mode.');
    }
    if (options.buildSummaryOnlyDiet && !options.buildSummaryOnly) {
      printAndFail('The option --build-summary-only-diet can be used only '
          'together with --build-summary-only.');
    }

    return options;
  }

  static String _getVersion() {
    try {
      // This is relative to bin/snapshot, so ../..
      String versionPath =
          Platform.script.resolve('../../version').toFilePath();
      File versionFile = new File(versionPath);
      return versionFile.readAsStringSync().trim();
    } catch (_) {
      // This happens when the script is not running in the context of an SDK.
      return "<unknown>";
    }
  }

  static CommandLineOptions _parse(List<String> args) {
    args = preprocessArgs(PhysicalResourceProvider.INSTANCE, args);

    bool verbose = args.contains('-v') || args.contains('--verbose');
    bool hide = !verbose;

    var parser = new ArgParser(allowTrailingOptions: true);
    defineAnalysisArguments(parser, hide: hide);
    parser
      ..addFlag('batch',
          abbr: 'b',
          help: 'Read commands from standard input (for testing).',
          defaultsTo: false,
          negatable: false)
      ..addOption('format',
          help: 'Specifies the format in which errors are displayed.')
      ..addFlag('machine',
          help: 'Print errors in a format suitable for parsing (deprecated).',
          defaultsTo: false,
          negatable: false)
      ..addFlag('version',
          help: 'Print the analyzer version.',
          defaultsTo: false,
          negatable: false)
      ..addFlag('lints',
          help: 'Show lint results.', defaultsTo: false, negatable: false)
      ..addFlag('no-hints',
          help: 'Do not show hint results.',
          defaultsTo: false,
          negatable: false)
      ..addFlag('disable-cache-flushing', defaultsTo: false, hide: true)
      ..addFlag(ignoreUnrecognizedFlagsFlag,
          help: 'Ignore unrecognized command line flags.',
          defaultsTo: false,
          negatable: false)
      ..addFlag('fatal-hints',
          help: 'Treat hints as fatal.', defaultsTo: false, negatable: false)
      ..addFlag('fatal-warnings',
          help: 'Treat non-type warnings as fatal.',
          defaultsTo: false,
          negatable: false)
      ..addFlag('fatal-lints',
          help: 'Treat lints as fatal.', defaultsTo: false, negatable: false)
      ..addFlag('package-warnings',
          help: 'Show warnings from package: imports.',
          defaultsTo: false,
          negatable: false)
      ..addFlag('show-package-warnings',
          help: 'Show warnings from package: imports (deprecated).',
          defaultsTo: false,
          negatable: false)
      ..addFlag('warnings',
          help: 'Show warnings from SDK imports.',
          defaultsTo: false,
          negatable: false)
      ..addFlag('show-sdk-warnings',
          help: 'Show warnings from SDK imports (deprecated).',
          defaultsTo: false,
          negatable: false)
      ..addOption('x-package-warnings-prefix',
          help:
              'Show warnings from package: imports that match the given prefix',
          hide: true)
      ..addOption('x-perf-report',
          help: 'Writes a performance report to the given file (experimental).')
      ..addFlag('help',
          abbr: 'h',
          help: 'Display this help message.\n'
              'Add --verbose to show hidden options.',
          defaultsTo: false,
          negatable: false)
      ..addFlag('verbose',
          abbr: 'v', defaultsTo: false, help: 'Verbose output.')
      ..addOption('url-mapping',
          help: '--url-mapping=libraryUri,/path/to/library.dart directs the '
              'analyzer to use "library.dart" as the source for an import '
              'of "libraryUri".',
          allowMultiple: true,
          splitCommas: false)
      //
      // Build mode.
      //
      ..addFlag('persistent_worker',
          help: 'Enable Bazel persistent worker mode.',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addOption('build-analysis-output',
          help:
              'Specifies the path to the file where analysis results should be written.',
          hide: hide)
      ..addFlag('build-mode',
          // TODO(paulberry): add more documentation.
          help: 'Enable build mode.',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addOption('build-summary-input',
          help: 'Path to a summary file that contains information from a '
              'previous analysis run.  May be specified multiple times.',
          allowMultiple: true,
          hide: hide)
      ..addOption('build-summary-output',
          help: 'Specifies the path to the file where the full summary '
              'information should be written.',
          hide: hide)
      ..addOption('build-summary-output-semantic',
          help: 'Specifies the path to the file where the semantic summary '
              'information should be written.',
          hide: hide)
      ..addFlag('build-summary-only',
          help: 'Disable analysis (only generate summaries).',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addFlag('build-summary-only-ast',
          help: 'deprecated -- Generate summaries using ASTs.',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addFlag('build-summary-only-diet',
          help: 'Diet parse function bodies.',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addFlag('build-summary-exclude-informative',
          help: 'Exclude @informative information (docs, offsets, etc).  '
              'Deprecated: please use --build-summary-output-semantic instead.',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addFlag('build-suppress-exit-code',
          help: 'Exit with code 0 even if errors are found.',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      //
      // Hidden flags.
      //
      ..addFlag('enable-async',
          help: 'Enable support for the proposed async feature.',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addFlag('enable-enum',
          help: 'Enable support for the proposed enum feature.',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addFlag('enable-conditional-directives',
          help:
              'deprecated -- Enable support for conditional directives (DEP 40).',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addFlag('enable-null-aware-operators',
          help: 'Enable support for null-aware operators (DEP 9).',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addFlag('enable-new-task-model',
          help: 'deprecated -- Ennable new task model.',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addFlag('log',
          help: 'Log additional messages and exceptions.',
          defaultsTo: false,
          negatable: false,
          hide: hide)
      ..addFlag('enable_type_checks',
          help: 'Check types in constant evaluation.',
          defaultsTo: false,
          negatable: false,
          hide: hide);

    try {
      // TODO(scheglov) https://code.google.com/p/dart/issues/detail?id=11061
      args =
          args.map((String arg) => arg == '-batch' ? '--batch' : arg).toList();
      if (args.contains('--$ignoreUnrecognizedFlagsFlag')) {
        args = filterUnknownArguments(args, parser);
      }
      var results = parser.parse(args);

      // Persistent worker.
      if (args.contains('--persistent_worker')) {
        bool validArgs;
        if (!args.contains('--build-mode')) {
          validArgs = false;
        } else if (args.length == 2) {
          validArgs = true;
        } else if (args.length == 4 && args.contains('--dart-sdk')) {
          validArgs = true;
        } else {
          validArgs = false;
        }
        if (!validArgs) {
          printAndFail('The --persistent_worker flag should be used with and '
              'only with the --build-mode flag, and possibly the --dart-sdk '
              'option. Got: $args');
          return null; // Only reachable in testing.
        }
        return new CommandLineOptions._fromArgs(results);
      }

      // Help requests.
      if (results['help']) {
        _showUsage(parser);
        exitHandler(0);
        return null; // Only reachable in testing.
      }
      // Batch mode and input files.
      if (results['batch']) {
        if (results.rest.isNotEmpty) {
          errorSink.writeln('No source files expected in the batch mode.');
          _showUsage(parser);
          exitHandler(15);
          return null; // Only reachable in testing.
        }
      } else if (results['persistent_worker']) {
        if (results.rest.isNotEmpty) {
          errorSink.writeln(
              'No source files expected in the persistent worker mode.');
          _showUsage(parser);
          exitHandler(15);
          return null; // Only reachable in testing.
        }
      } else if (results['version']) {
        outSink.write('$_binaryName version ${_getVersion()}');
        exitHandler(0);
        return null; // Only reachable in testing.
      } else {
        if (results.rest.isEmpty && !results['build-mode']) {
          _showUsage(parser);
          exitHandler(15);
          return null; // Only reachable in testing.
        }
      }
      return new CommandLineOptions._fromArgs(results);
    } on FormatException catch (e) {
      errorSink.writeln(e.message);
      _showUsage(parser);
      exitHandler(15);
      return null; // Only reachable in testing.
    }
  }

  static _showUsage(parser) {
    errorSink
        .writeln('Usage: $_binaryName [options...] <libraries to analyze...>');
    errorSink.writeln(parser.getUsage());
    errorSink.writeln('');
    errorSink.writeln(
        'For more information, see http://www.dartlang.org/tools/analyzer.');
  }
}
