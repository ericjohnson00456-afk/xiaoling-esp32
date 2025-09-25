<template>
  <sonic-view :peak="peak" :level="level" :period="500" />
  <div :class="$style.content">
    <simple-view :state="state" :accept-exts="ACCEPT_EXTS" @file="handleFile" @connect="connect" @flash="flash"
      @reset="reset" @start="oneClick" @clear="clear" />
  </div>
</template>

<script lang="ts" setup>
import { computed, reactive } from 'vue';
import { message } from 'ant-design-vue';

import SonicView from '@/components/SonicView.vue';
import SimpleView from '@/routes/SimpleView.vue';

import useTotalProgress from '@/composables/useTotalProgress';

import readZip from '@/unpack/readZip';
import readUf2 from '@/unpack/readUf2';
import readBin from './unpack/readBin';
import ESPTool from '@/esptool';

import type { IState } from '@/types/state';
import type { IESPDevice, IFlashProgress } from '@/esptool';

const ACCEPT_EXTS = ['.zip', '.bin', '.uf2'];

const MAX_FILE_SIZE = 16 * 1024 * 1024;

const state = reactive<IState>({
  stage: 'idle',
  firmware: undefined,
  device: undefined,
  flashArgs: undefined,
  progress: undefined,
});

const esp = new ESPTool();

esp.on('connect', (device: IESPDevice) => {
  state.device = device;
  console.log(`Connected: ${device.description}`);
  message.success(`已连接：${device.description}`);
});

esp.on('disconnect', () => {
  state.device = undefined;
});

esp.on('progress', (progress: IFlashProgress) => {
  state.progress = progress;
});

async function handleFile(file: File): Promise<void> {
  if (file.size >= MAX_FILE_SIZE) {
    message.error(`文件过大: ${Math.round(file.size / 1024 / 1024)} MB`);
    return;
  }

  state.progress = undefined;

  const name = file.name.toLocaleLowerCase();
  if (name.endsWith('.zip')) {
    state.flashArgs = await readZip(file);
  } else if (name.endsWith('.uf2') || name == 'uf2.bin') {
    state.flashArgs = await readUf2(file);
  } else if (name.endsWith('.bin')) {
    state.flashArgs = await readBin(file);
  }

  if (!state.flashArgs) {
    message.error('该文件不是一个合法的固件包');
    return;
  }

  state.firmware = file;
}

async function connect(): Promise<boolean> {
  if (state.device) {
    return true;
  }
  try {
    state.stage = 'connecting';
    const serial = await navigator.serial.requestPort();
    await esp.open(serial);
    return true;
  } catch (e) {
    message.error('设备打开失败');
    return false;
  } finally {
    state.stage = 'idle';
  }
}

async function flash(reset = false): Promise<boolean> {
  if (!state.flashArgs) {
    return false;
  }
  try {
    state.stage = 'flashing';
    await esp.flash(state.flashArgs, reset);
    return true;
  } catch (e) {
    console.error(e);
    message.error('升级失败');
    state.progress = undefined;
    return false;
  } finally {
    state.stage = 'idle';
  }
}

async function reset(): Promise<void> {
  await esp.reset();
}

async function oneClick(): Promise<void> {
  if (!await connect()) {
    return;
  }
  await flash(true);
  await esp.close();
  console.log('done');
}

function clear(): void {
  state.progress = undefined;
}

const progress = useTotalProgress(state);

const peak = computed(() => {
  if (state.stage == 'flashing') return 0.7;
  else if (state.stage == 'connecting') return 0.4;
  else if (progress.value != null && progress.value >= 100) return 0;
  else return 0.2;
});

const level = computed(() => {
  if (progress.value == null) {
    return 0.02;
  } else {
    return 0.02 + (progress.value / 100) * 1.1;
  }
});
</script>

<style lang="scss" module>
:global(html),
:global(body) {
  margin: 0;
  padding: 0;
}

.content {
  width: 100vw;
  height: 80vh;
  min-height: 500px;
  display: flex;
  flex-direction: column;
  justify-content: center;
  align-items: center;
}
</style>
