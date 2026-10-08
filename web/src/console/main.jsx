import { render } from 'preact';
import { App } from './App';
import { connect } from './live';
import './console.css';

render(<App />, document.getElementById('app'));
connect();
