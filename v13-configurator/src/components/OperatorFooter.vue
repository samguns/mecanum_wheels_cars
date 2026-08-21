<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { invoke } from '@tauri-apps/api/core'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const name = ref('anonymous')
const email = ref('')

onMounted(async () => {
  try {
    const profile = await invoke<{ display_name: string; email?: string | null }>('operator_get')
    name.value = profile.display_name
    email.value = profile.email ?? ''
  } catch {
    /* outside Tauri */
  }
})

async function save() {
  await invoke('operator_set', {
    args: { display_name: name.value, email: email.value || null },
  })
}

async function endSession() {
  await session.disconnect()
}
</script>

<template>
  <footer class="footer">
    <label>Operator <input v-model="name" /></label>
    <label>Email <input v-model="email" /></label>
    <button type="button" @click="save">Save identity</button>
    <button type="button" class="ghost" @click="endSession">End session</button>
  </footer>
</template>

<style scoped>
.footer {
  margin-top: auto;
  padding-top: 16px;
  font-size: 12px;
  color: #8b959f;
}
label {
  display: block;
  margin: 6px 0;
}
input {
  width: 100%;
  background: #0b0f14;
  color: inherit;
  border: 1px solid #2a333d;
}
button {
  margin-top: 8px;
  margin-right: 6px;
  background: #16323a;
  color: #4fd6c1;
  border: 0;
  border-radius: 6px;
  padding: 4px 8px;
}
.ghost {
  background: transparent;
  border: 1px solid #2a333d;
  color: inherit;
}
</style>
