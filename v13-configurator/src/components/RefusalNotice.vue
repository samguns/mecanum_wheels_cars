<script setup lang="ts">
defineProps<{ reason: string; motor?: number }>()
</script>

<template>
  <p class="refusal">
    Controller refused{{ motor ? ` on motor ${motor}` : '' }}: {{ reason }}
  </p>
</template>

<style scoped>
.refusal {
  color: #e3b341;
  font-size: 13px;
}
</style>
