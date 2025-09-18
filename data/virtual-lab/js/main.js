import { mountOscilloscope } from './oscilloscope.js';
import { mountMultimeter } from './multimeter.js';
import { mountFunctionGenerator } from './functionGenerator.js';
import { mountMathZone } from './mathZone.js';
import { loadIoCatalog } from './ioCatalog.js';

document.addEventListener('DOMContentLoaded', async () => {
  const catalog = await loadIoCatalog();
  const { inputs, outputs, multimeterChannels, error } = catalog;

  mountOscilloscope(document.getElementById('oscilloscope'), { inputs, error });
  mountMultimeter(document.getElementById('multimeter'), {
    inputs,
    error,
    meterChannels: Array.isArray(multimeterChannels) ? multimeterChannels : []
  });
  mountFunctionGenerator(document.getElementById('function-generator'), { outputs, error });
  mountMathZone(document.getElementById('math-zone'));
});
