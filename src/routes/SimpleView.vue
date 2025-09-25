<template>
  <h1 :class="$style.title">小聆 AI 在线升级</h1>
  <Transition name="slide-up" mode="out-in">
    <div :class="[$style.main, $style.upload]" v-if="progress == null">
      <div :style="{ marginBottom: '20px' }">
        <a-radio-group v-model:value="source" button-style="solid">
          <a-radio-button value="github">在线升级</a-radio-button>
          <a-radio-button value="local">本地文件</a-radio-button>
        </a-radio-group>
      </div>
      <a-space v-if="source == 'github'" direction="vertical" :style="{ width: '100%' }" size="large">
        <a-space align="center">
          <span :style="{ width: '4em' }">版本：</span>
          <a-select v-model:value="tag" :style="{ width: '250px' }" size="large" placeholder="选择固件版本">
            <a-select-option v-for="r in releases" :key="r.tag" :value="r.tag">
              <a-flex justify="space-between" align="center" style="width: 100%;">
                {{ r.name }}
                <a-tag v-if="r.kind == 'latest'" color="green">Latest</a-tag>
                <a-tag v-else-if="r.kind == 'prerelease'" color="orange">Pre-release</a-tag>
              </a-flex>
            </a-select-option>
          </a-select>
          <span :style="{ marginLeft: '0.5em' }" v-if="release">
            <a :href="release.url" target="_blank">发行说明</a>
          </span>
        </a-space>
        <a-space align="center">
          <span :style="{ width: '4em' }">板型：</span>
          <a-select v-model:value="board" :style="{ width: '400px' }" size="large" placeholder="选择板型" show-search
            :disabled="!release">
            <a-select-option v-for="a in release?.assets ?? []" :key="a.board" :value="a.board">
              {{ a.board }}
            </a-select-option>
          </a-select>
        </a-space>
        <div :style="{ width: '200px' }" v-if="downloading !== undefined">
          正在准备固件 <a-progress size="small" :percent="downloading" />
        </div>
      </a-space>
      <div v-else="source=='local'" :style="{ height: '200px' }">
        <a-upload-dragger :accept="acceptExts.join(',')" :showUploadList="false" :customRequest="handleFile">
          <p class="ant-upload-drag-icon">
            <file-zip-outlined v-if="firmware" />
            <inbox-outlined v-else />
          </p>
          <p class="ant-upload-text" :class="$style.file" v-if="firmware">{{ firmware.name }}</p>
          <p class="ant-upload-text" v-else>点击选择或将固件包拖放到此处</p>
        </a-upload-dragger>
      </div>
    </div>
    <div :class="[$style.main, $style.progress]" v-else-if="completed">升级完成</div>
    <div :class="[$style.main, $style.progress]" v-else>{{ Math.floor(progress || 0) }}%</div>
  </Transition>
  <div :class="$style.buttons">
    <Transition name="fade" mode="out-in">
      <template v-if="stage != 'flashing'">
        <a-button v-if="completed" size="large" type="default" ghost @click="handleClear" :class="$style.reset">
          再次升级
        </a-button>
        <a-button v-else size="large" type="primary" :disabled="!asset && !firmware"
          :loading="['connecting', 'flashing'].includes(stage)" @click="handleStart">
          开始升级
        </a-button>
      </template>
    </Transition>
  </div>
</template>

<script lang="ts" setup>
import { computed, onMounted, ref, toRefs } from 'vue';
import { message } from 'ant-design-vue';
import { InboxOutlined, FileZipOutlined } from '@ant-design/icons-vue';

import useTotalProgress from '@/composables/useTotalProgress';

import type { IState } from '@/types/state';

const props = defineProps<{
  state: IState;
  acceptExts: string[];
}>();

const emit = defineEmits<{
  (e: 'file', file: File): void;
  (e: 'connect'): void;
  (e: 'reset'): void;
  (e: 'flash'): void;
  (e: 'start'): void;
  (e: 'clear'): void;
}>();

const { stage, firmware } = toRefs(props.state);
const progress = useTotalProgress(props.state);
const completed = computed(() => progress.value == 100);

function handleFile({ file }: { file: File }): void {
  emit('file', file);
}

const downloading = ref<number | undefined>(undefined);

async function handleStart(): Promise<void> {
  if (source.value == 'github') {
    if (!asset.value) {
      return;
    }

    const resp = await fetch(asset.value.url);
    if (!resp.ok) {
      message.error(`固件下载失败: ${resp.status} ${resp.statusText}`);
      return;
    }

    const length = Number(resp.headers.get('Content-Length') ?? '0');
    const reader = resp.body?.getReader();
    if (!reader) {
      message.error('固件读取失败');
      return;
    }

    let received = 0;
    const chunks = [];
    while (true) {
      const { done, value } = await reader.read();
      if (done) {
        break;
      }
      if (value) {
        chunks.push(value);
        received += value.length;
        if (length > 0) {
          downloading.value = Math.floor((received / length) * 100);
        }
      }
    }
    downloading.value = undefined;

    const blob = new Blob(chunks, { type: 'application/octet-stream' });

    const file = new File([blob], asset.value.name, { type: blob.type });
    emit('file', file);
  }

  emit('start');
}

function handleClear(): void {
  emit('clear');
}

const source = ref<'github' | 'local'>('github');
const tag = ref<string>();
const board = ref<string>();

const releases = ref<{
  name: string;
  kind: 'latest' | 'prerelease' | null;
  tag: string;
  url: string;
  assets: {
    name: string;
    board: string;
    url: string;
  }[];
}[]>([]);

onMounted(async () => {
  releases.value = await (await fetch('https://raw.githubusercontent.com/ericjohnson00456-afk/xiaoling-esp32_releases/refs/heads/index/releases.json')).json();
  tag.value = releases.value.find(r => r.kind == 'latest')?.tag;
});

const release = computed(() => tag.value ? releases.value.find((r) => r.tag == tag.value) : undefined);
const asset = computed(() => board.value ? release.value?.assets.find((a) => a.board == board.value) : undefined);
</script>

<style lang="scss" module>
.title {
  font-size: 50px;
  font-weight: 100;
  line-height: 1;
  margin-bottom: 50px;
}

.main {
  width: 90%;
  height: 300px;
  max-width: 800px;
}

.upload {
  :global(.ant-upload-btn) {
    display: flex !important;
    flex-direction: column;
    justify-content: center;
    padding: 24px 16px 16px;
  }

  .file {
    font-family: monospace;
    text-overflow: ellipsis;
    overflow: hidden;
    padding: 0 16px;
  }
}

.progress {
  display: flex;
  flex-direction: column;
  justify-content: center;
  align-items: center;
  max-width: unset;
  font-size: 100px;
  font-weight: 100;
}

.buttons {
  margin-top: 40px;
  height: 50px;
}

.reset:global(.ant-btn.ant-btn-background-ghost) {
  opacity: 0.8;

  &:hover,
  &:focus {
    color: white;
    border-color: white;
    opacity: 1.0;
  }
}

:global(.slide-up-enter-active),
:global(.slide-up-leave-active) {
  transition: all 0.25s ease-out;
}

:global(.slide-up-enter-from) {
  opacity: 0;
  transform: translateY(30px);
}

:global(.slide-up-leave-to) {
  opacity: 0;
  transform: translateY(-30px);
}

:global(.fade-enter-active),
:global(.fade-leave-active) {
  transition: opacity 0.5s ease;
}

:global(.fade-enter-from),
:global(.fade-leave-to) {
  opacity: 0;
}
</style>
