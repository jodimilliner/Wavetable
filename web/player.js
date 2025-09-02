export async function createWasmDSPNode(audioContext) {
  if (!audioContext?.audioWorklet) {
    throw new Error('AudioWorklet not supported');
  }
  await audioContext.audioWorklet.addModule('worklet/synth-processor.js');
  const node = new AudioWorkletNode(audioContext, 'synth-processor', {
    numberOfInputs: 0,
    numberOfOutputs: 1,
    outputChannelCount: [2],
    parameterData: { gain: 1.0 }
  });
  node.connect(audioContext.destination);
  return node;
}

