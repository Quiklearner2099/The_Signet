<div align="center">

![The Signet banner](data/the_signet_banner_1600x400.png)


[![GitHub release](https://img.shields.io/github/v/release/Quiklearner2099/The_Signet)](https://github.com/Quiklearner2099/The_Signet/releases)
[![Arduino](https://img.shields.io/badge/Platform-Arduino-00979D.svg)](https://www.arduino.cc/)
[![ESP32-C6](https://img.shields.io/badge/MCU-ESP32--C6-red.svg)](https://www.espressif.com/)
[![License: MIT](https://img.shields.io/badge/Software-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![License: CERN-OHL-P-2.0](https://img.shields.io/badge/Hardware-CERN--OHL--P%202.0-blue.svg)](https://ohwr.org/cern_ohl_p_v2.txt)

<h1>BIG BROTHER IS WATCHING YOU…<br/>Send him a message.</h1>

[![Steganography](https://img.shields.io/badge/Steganography-Morse%20Code-purple.svg)]()
[![Free Speech](https://img.shields.io/badge/Free%20Speech-Tool-1DA1F2.svg)]()


##  <strong>A Morse Beacon for Protest Messaging / Video Steganography</strong>


<p>
  <strong>The Signet</strong> is a small, WiFi configurable beacon designed to embed a message
  (<strong>encoded in Morse code</strong>)<br>into <strong>recorded video footage</strong>
  using a blinking light source.
</p>

</div>

<div align="center">

---

### **NEW TO THE SIGNET?**

<a href="https://quiklearner2099.github.io/The_Signet/Tutorial/tutorial_demo.html">
  <img src="https://img.shields.io/badge/%F0%9F%8E%93_INTERACTIVE_TUTORIAL-START_HERE-brightgreen?style=for-the-badge&labelColor=black" alt="Interactive Tutorial">
</a>

**Learn how to use The Signet in minutes with our step-by-step interactive guide!**

---

</div>

<h2>Feature Breakdown</h2>
<table>
<tr>
<th align="left" width="46%">Hardware</th>
<th align="left" width="54%">Firmware</th>
</tr>
<tr>
<td valign="top">
&bull; <strong>XIAO ESP32-C6 SoC</strong> - cheap, small, capable<br/>
&bull; <strong>Dual emitters</strong> - RGB LED + 850nm IR LED<br/>
&bull; <strong>500 mAh Li-Po</strong> with dis/charge protection<br/>
&bull; <strong>USB-C charging</strong> - can be used while charging<br/>
&bull; <strong>Magnetic mounting</strong> - for fast, easy placement<br/><br>
&bull; <strong>Designed for easy assembly and quick builds</strong><br/>
&bull; <strong>Snap-fit 3D-printable enclosure (2 pieces)</strong><br/>
&bull; <strong>Minimal soldering required</strong>

</td>
<td valign="top">
&bull; <strong>Plain text &#8594; International Morse code signaling<br/>
&bull; <strong>Language agnostic</strong> - enc/decodes the same worldwide<br/>
&bull; <strong>Visible / Discreet modes</strong> - selectable RGB / IR output<br/>
&bull; <strong>WiFi AP+CP</strong> - browser-based UI; no app required<br/>
&bull; <strong>No internet connectivity</strong> - operates fully offline<br/>
&bull; <strong>Auto WiFi shutdown after 90 seconds of UI inactivity</strong><br/>
&bull; <strong>"Stateless by Power" design</strong>; no messages stored<br/>
&bull; <strong>NO telemetry, NO accounts, NO bullshyte<strong>
</td>
</tr>
</table>

<hr/>

<h2>Output Modes</h2>

<ul>
  <li>
    <strong>Visible</strong> — uses an <strong>RGB LED</strong> for human-eye detection
    <em>(obvious in person, obvious on camera)</em>
  </li>
  <li>
    <strong>Discreet</strong> — uses an <strong>IR LED</strong> for low-visibility messaging
    <em>(typically not noticeable to most bystanders, but may still be captured on video depending on the camera)</em>
  </li>
</ul>

<hr/>

<h2>Why this exists</h2>

<h3>📣 Durable speech when platforms fail</h3>

<p>
  In many places, digital censorship and content suppression are rising.
  <strong>The Signet</strong> is built for situations where people need to communicate ideas in public—
  especially during peaceful demonstrations—without relying on platforms, audio, or live speech.
</p>

<p>
  Instead of shouting a message (which can provoke instant confrontation or get drowned out),
  <strong>The Signet</strong> lets a message persist inside the footage itself—so it can be discovered later
  in recordings from participants, journalists, or fixed cameras.
  The point is durability: the message travels with the video.
</p>

<blockquote>
  <strong>
    <strong>The Signet</strong> uses standard International Morse Code (ITU), ensuring global readability
    and compatibility with both human and machine decoding. A message embedded in footage can’t be muted, 
    algorithmically downranked, or “lost in the crowd” the same way a post can.
  </strong>
</blockquote>

<h3>🧾 Footage continuity &amp; tamper indication</h3>

<p>
  <strong>The Signet</strong> can also be used as a continuity marker for recorded footage.
  By embedding a continuous, time-ordered Morse message into the recording,
  the signal should remain consistent from start to finish.
</p>

<ul>
  <li>
    Unexpected gaps, jumps, or resets may indicate
    cuts, dropped frames, edits, or tampering.
  </li>
  <li>
    This can help viewers and investigators spot continuity breaks
    that might otherwise go unnoticed.
  </li>
</ul>

<hr/>

<h2>How this differs from watermarking</h2>

<p>
  While <strong>The Signet</strong> and watermarking both involve embedding information into video,
  they solve very different problems and operate in fundamentally different ways.
</p>

<h3>Watermarking</h3>

<ul>
  <li>Added <strong>after recording</strong> during editing or encoding</li>
  <li>Relies on digital pixel or compression manipulation</li>
  <li>Usually invisible and algorithmic</li>
  <li>Can often be removed or degraded through re-encoding</li>
  <li>Primarily serves ownership and copyright enforcement</li>
</ul>

<h3>The Signet</h3>

<ul>
  <li>Embeds information <strong>at the moment of recording</strong></li>
  <li>Uses optical signaling (visible or IR light)</li>
  <li>Independent of codecs, platforms, and formats</li>
  <li>Becomes part of the physical scene, not a digital layer</li>
  <li>Can reveal edits through continuity breaks</li>
</ul>

<blockquote>
  <strong>Watermarking alters the media.<br/>
  The Signet alters the scene.</strong>
</blockquote>

<hr/>

<h2>Privacy &amp; Security by Design</h2>

<ul>
  <li><strong>No app required</strong> — browser-based configuration only</li>
  <li><strong>No internet connectivity</strong> — never connects to cloud services</li>
  <li><strong>Temporary local Wi-Fi AP</strong> for configuration only</li>
  <li><strong>Automatic AP shutdown</strong> after 90 seconds of inactivity</li>
  <li><strong>No persistent logs or telemetry</strong></li>
  <li><strong>Stateless by power design</strong> — power cycling resets to default</li>
</ul>

<hr/>

<h2>Ethical Use</h2>

<p>
  The Signet is intended to support lawful, non-violent freedom of expression,
  documentation, and artistic or journalistic messaging.
  It must not be used to enable harm, harassment, intimidation, illegal activity,
  or to mislead people about what is being recorded or communicated.
</p>

## Support This Project

<a href="https://www.buymeacoffee.com/Quiklearner2099" target="_blank">
  <img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" height="50">
</a>
