# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

board_runner_args(openocd "--file-type=elf" "--verify")
include("${ZEPHYR_BASE}/boards/common/openocd.board.cmake")
