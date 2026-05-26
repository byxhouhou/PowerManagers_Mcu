# 单元测试 / Unit Tests

`power_manager_unit_test.c` 是一个轻量级 C 单元测试文件，用 fake FreeRTOS 和 fake board port 测试电源状态机核心逻辑。

`power_manager_unit_test.c` is a lightweight C unit test file that uses fake FreeRTOS and fake board port implementations to test the core power state machine logic.

## 覆盖场景 / Covered Cases

- `SLEEP -> WAKEUP -> WORK` 正常唤醒和工作迁移。  
  Normal `SLEEP -> WAKEUP -> WORK` transition.
- `WORK -> SHUTDOWN_PREPARE -> SLEEP`，无 required 模块时正常关机。  
  Normal shutdown when no required module blocks sleep.
- `SHUTDOWN_PREPARE` 等待 CAN/APP 模块 ready。  
  Shutdown preparation waits for CAN/APP ready confirmations.
- 关机 ready 超过 `POWER_MANAGER_SHUTDOWN_FORCE_TIMEOUT_TICKS` 后强制进入 `SLEEP`。  
  Forced sleep after `POWER_MANAGER_SHUTDOWN_FORCE_TIMEOUT_TICKS`.
- 外设电源管理在 `WORK` 中 resume，在 `SLEEP` 中 suspend。  
  Peripheral PM resumes in `WORK` and suspends in `SLEEP`.

## 语法检查 / Syntax Check

当前 Windows 环境只有 ARM GCC，因此已使用以下方式做语法检查：

The current Windows environment only has ARM GCC, so syntax checking is done with:

```powershell
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror `
  -Iinclude -Itest_compile\freertos_stubs `
  -fsyntax-only `
  tests\power_manager_unit_test.c
```

## 本机运行 / Native Run

在有主机 GCC 的环境中，可以直接编译运行：

On a machine with native GCC, build and run it directly:

```sh
gcc -std=c99 -Wall -Wextra -Werror \
  -Iinclude -Itest_compile/freertos_stubs \
  tests/power_manager_unit_test.c \
  -o power_manager_unit_test

./power_manager_unit_test
echo $?
```

返回值为 `0` 表示全部通过。  
Exit code `0` means all tests passed.

运行时会打印每个测试项的 `[RUN]` / `[PASS]` / `[FAIL]`，最后输出汇总和“全部测试通过”。  
At runtime it prints `[RUN]` / `[PASS]` / `[FAIL]` for each case, followed by a summary and an all-passed message.

## Windows portable GCC

本工作区可使用 `tools\w64devkit\bin\gcc.exe`。在 Windows cmd 中可先运行：

This workspace can use `tools\w64devkit\bin\gcc.exe`. In Windows cmd, run:

```bat
tools\use-gcc.cmd
```
