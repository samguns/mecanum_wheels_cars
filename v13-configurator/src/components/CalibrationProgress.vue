<script setup lang="ts">
defineProps<{
  motor: number
  stage: string
  percent: number
  energised: boolean
  phase: string
  reason: string | null
}>()
</script>

<template>
  <section class="panel">
    <h2>Calibration progress</h2>
    <p class="reported">
      Motor {{ motor }} · {{ stage }} · {{ percent }}% · phase {{ phase }}
    </p>
    <p v-if="energised" class="hot">Motor energised</p>
    <p v-else class="reported">Motor de-energised</p>
    <p v-if="reason" class="warn">{{ reason }}</p>
  </section>
</template>

<style scoped>
.panel {
  background: #12181f;
  border: 1px solid #1d242c;
  border-radius: 8px;
  padding: 16px;
  margin: 16px 0;
}
.hot {
  color: #ff8a8a;
  font-weight: 600;
}
.warn {
  color: #e3b341;
}
.reported {
  color: #c5cdd4;
}
</style>
