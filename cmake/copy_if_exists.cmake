
if(NOT DEFINED LP_COPY_SRC OR NOT DEFINED LP_COPY_DST)
  message(FATAL_ERROR "copy_if_exists.cmake requires -DLP_COPY_SRC= and -DLP_COPY_DST=")
endif()

# PDBs only exist for configs built with debug info (e.g. not plain Release),
# so a plain copy_if_different would fail the build when there is none.
if(EXISTS "${LP_COPY_SRC}")
  file(COPY_FILE "${LP_COPY_SRC}" "${LP_COPY_DST}" ONLY_IF_DIFFERENT)
endif()
