add_mlir_library(
  # Library called MyExtension.
  MyExtensionCh3

  # Built from the following source files.
  MyExtension.cpp

  # Make includes visible without top-level path.
  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/examples/transform/Ch3/include

  # Make sure ODS declaration and definitions are generated before compiling this.
  DEPENDS
  MyExtensionCh3IncGen

  # Link in the transform dialect, an all generated dialects.
  LINK_LIBS PUBLIC
  MLIRTransformDialect
  MLIRFuncDialect
  MLIRSCFDialect
)
