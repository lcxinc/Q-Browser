export interface StableIoTestCallbacks {
  afterInputOpen?(filePath: string): void | Promise<void>;
  afterInputInitialStat?(filePath: string): void | Promise<void>;
  betweenInputSnapshots?(filePath: string): void | Promise<void>;
  afterInputLockReady?(filePath: string, processId: number): void | Promise<void>;
  beforeInputLockReadyAcceptance?(filePath: string): void | Promise<void>;
  beforeDirectoryLockReadyAcceptance?(directoryPath: string): void | Promise<void>;
  lockHelperReadyTimeoutMs?: number;
  afterLockHelperSpawn?(targetPath: string, attempt: number, processId: number): void | Promise<void>;
  afterFirstDirectoryLockReady?(outputParent: string, reportParent: string, processId?: number): void | Promise<void>;
  beforeLockRelease?(targetPath: string): void | Promise<void>;
  afterGenerationStaged?(outputPath: string, reportPath: string): void | Promise<void>;
  beforeOutputCommit?(outputPath: string, reportPath: string): void | Promise<void>;
  afterOutputPublish?(outputPath: string, reportPath: string): void | Promise<void>;
  beforeOutputRollback?(outputPath: string, reportPath: string): void | Promise<void>;
  beforeReportPublish?(outputPath: string, reportPath: string): void | Promise<void>;
}

let callbacks: StableIoTestCallbacks = {};

export function setStableIoTestCallbacks(value: StableIoTestCallbacks = {}): void {
  callbacks = { ...value };
}

export async function ioCheckpoint(name: string, ...values: unknown[]): Promise<void> {
  const callback = callbacks[name as keyof StableIoTestCallbacks] as ((...args: unknown[]) => void | Promise<void>) | undefined;
  await callback?.(...values);
}

export function ioReadyTimeout(defaultMilliseconds: number): number {
  return callbacks.lockHelperReadyTimeoutMs ?? defaultMilliseconds;
}
