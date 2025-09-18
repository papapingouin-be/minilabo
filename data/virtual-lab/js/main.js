import { mountOscilloscope } from './oscilloscope.js';
import { mountMultimeter } from './multimeter.js';
import { mountFunctionGenerator } from './functionGenerator.js';
import { mountMathZone } from './mathZone.js';

document.addEventListener('DOMContentLoaded', () => {
  mountOscilloscope(document.getElementById('oscilloscope'));
  mountMultimeter(document.getElementById('multimeter'));
  mountFunctionGenerator(document.getElementById('function-generator'));
  mountMathZone(document.getElementById('math-zone'));
});
