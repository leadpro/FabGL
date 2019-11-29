#include "machine.h"

#include "ROM/kernal_rom.h"
#include "ROM/basic_rom.h"
#include "ROM/char_rom.h"




Machine::Machine()
  : m_CPU(this),
    m_VIA1(this, 1, &Machine::VIA1PortOut, &Machine::VIA1PortIn),
    m_VIA2(this, 2, &Machine::VIA2PortOut, &Machine::VIA2PortIn),
    m_VIC(this)
{
  m_RAM1K    = new uint8_t[0x0400];
  m_RAM4K    = new uint8_t[0x1000];
  m_RAMColor = new uint8_t[0x0400];

  m_expRAM[0] = m_expRAM[1] = m_expRAM[2] = m_expRAM[3] = m_expRAM[4] = nullptr;
  m_expROM[0] = m_expROM[1] = m_expROM[2] = m_expROM[3] = nullptr;

  reset();
}


Machine::~Machine()
{
  delete [] m_RAM1K;
  delete [] m_RAM4K;
  delete [] m_RAMColor;

  for (int i = 0; i < 4; ++i)
    delete [] m_expRAM[i];
}


void Machine::reset()
{
  #if DEBUGMSG
  printf("Reset\n");
  #endif

  m_NMI           = false;
  m_lastSyncCycle = 0;
  m_typingString  = nullptr;
  m_lastSyncTime  = 0;

  VIA1().reset();
  VIA2().reset();

  VIC().reset();

  // TODO: check these!
  VIA1().setCA1(1);   // restore high (pulled up)
  VIA1().setPA(0x7E);
  VIA1().setPB(0xFF);

  resetJoy();
  resetKeyboard();

  m_cycle = m_CPU.Reset();
}


// 0: 3K expansion (0x0400 - 0x0fff)
// 1: 8K expansion (0x2000 - 0x3fff)
// 2: 8K expansion (0x4000 - 0x5fff)
// 3: 8K expansion (0x6000 - 0x7fff)
// 4: 8K expansion (0xA000 - 0xBfff)
void Machine::enableRAMBlock(int block, bool enabled)
{
  static const uint16_t BLKSIZE[] = { 0x0c00, 0x2000, 0x2000, 0x2000, 0x2000 };
  if (enabled && !m_expRAM[block]) {
    m_expRAM[block] = new uint8_t[BLKSIZE[block]];
  } else if (!enabled && m_expRAM[block]) {
    delete [] m_expRAM[block];
    m_expRAM[block] = nullptr;
  }
}


void Machine::setRAMExpansion(RAMExpansion value)
{
  static const uint8_t CONFS[RAM_35K + 1][5] = { { 1, 0, 0, 0, 0 },   // RAM_3K
                                                 { 0, 1, 0, 0, 0 },   // RAM_8K
                                                 { 0, 1, 1, 0, 0 },   // RAM_16K
                                                 { 0, 1, 1, 1, 0 },   // RAM_24K
                                                 { 1, 1, 1, 1, 0 },   // RAM_27K
                                                 { 0, 1, 1, 1, 1 },   // RAM_32K
                                                 { 1, 1, 1, 1, 1 },   // RAM_35K
                                               };
  for (int i = 0; i < 5; ++i)
    enableRAMBlock(i, CONFS[value][i]);
}


// Address can be: 0x2000, 0x4000, 0x6000 or 0xA000. -1 = auto
// block 0 : 0x2000
// block 1 : 0x4000
// block 2 : 0x6000
// block 3 : 0xA000
// If size is >4096 or >8192 then first bytes are discarded
void Machine::setCartridge(uint8_t const * data, size_t size, bool reset, int address)
{
  // get from data or default to 0xa000
  if (address == -1 && (size == 4098 || size == 8194)) {
    address = data[0] | (data[1] << 8);
    size -= 2;
    data += 2;
  }
  int block = (address == 0x2000 ? 0 : (address == 0x4000 ? 1 : (address == 0x6000 ? 2 : 3)));
  for (; size != 4096 && size != 8192; --size)
    ++data;
  m_expROM[block] = data;
  if (reset)
    this->reset();
}


void Machine::resetKeyboard()
{
  for (int row = 0; row < 8; ++row)
    for (int col = 0; col < 8; ++col)
      m_KBD[row][col] = 0;
}


int Machine::run()
{
  int runCycles = 0;
  while (runCycles < MOS6561::CyclesPerFrame) {

    int cycles = m_CPU.Run();

    // update timers, current scanline, check interrupts...
    for (int c = 0; c < cycles; ++c) {

  ///*
      // VIA1
      m_VIA1.tick();
      bool NMI = m_VIA1.interrupt();  // true = NMI request
      // NMI happens only on transition high->low (that is NMI=true m_NMI=false)
      if (NMI && !m_NMI)
        cycles += m_CPU.NMI();
      m_NMI = NMI;
  //*/

  ///*
      // VIA2
      m_VIA2.tick();
      bool IRQ = m_VIA2.interrupt();  // true = IRQ request
      if (IRQ && m_CPU.IRQEnabled())
        cycles += m_CPU.IRQ();
  //*/

  ///*
      // VIC
      m_VIC.tick();
  //*/

    }

    runCycles += cycles;
  }

  m_cycle += runCycles;

  handleCharInjecting();
  syncTime();

  return runCycles;
}


void Machine::handleCharInjecting()
{
  // char injecting?
  while (m_typingString) {
    int kbdBufSize = busRead(0x00C6);   // $00C6 = number of chars in keyboard buffer
    if (kbdBufSize < busRead(0x0289)) { // $0289 = maximum keyboard buffer size
      busWrite(0x0277 + kbdBufSize, *m_typingString++); // $0277 = keyboard buffer
      busWrite(0x00C6, ++kbdBufSize);
      if (!*m_typingString)
        m_typingString = nullptr;
    } else
      break;
  }
}


// delay number of cycles elapsed since last call to syncTime()
void Machine::syncTime()
{
  //uint64_t t1 = SDL_GetPerformanceCounter() + (m_cycle - m_lastSyncCycle) * 1000 /*1108*/;  // ns
  //while (SDL_GetPerformanceCounter() < t1)
  //  ;
  /*
  int diff = (int) (SDL_GetPerformanceCounter() - m_lastSyncTime);
  int delayNS = (m_cycle - m_lastSyncCycle) * 1108 - diff;
  if (delayNS > 0 && delayNS < 30000000) {
    const timespec rqtp = { 0, delayNS };
    nanosleep(&rqtp, nullptr);
  }

  m_lastSyncCycle = m_cycle;
  m_lastSyncTime  = SDL_GetPerformanceCounter();
  */
}


// just do change PC
void Machine::go(int addr)
{
  m_CPU.setPC(addr);
}


// only addresses restricted to 6561
uint8_t Machine::vicBusRead(uint16_t addr)
{
  int addr_hi = (addr >> 8) & 0xff; // 256B blocks
  int block   = (addr >> 12) & 0xf; // 4K blocks

  // 1K RAM (0000-03FF)
  if (addr < 0x400)
    return m_RAM1K[addr];

  // 4K RAM (1000-1FFF)
  else if (block == 1)
    return m_RAM4K[addr & 0xFFF];

  // 4K ROM (8000-8FFF)
  else if (block == 8)
    return char_rom[addr & 0xfff];

  // 1Kx4 RAM (9400-97FF)
  else if (addr_hi >= 0x94 && addr_hi <= 0x97)
    return m_RAMColor[addr & 0x3ff] & 0x0f;

  // unwired address returns high byte of the address
  return addr >> 8;
}


uint8_t Machine::busRead(uint16_t addr)
{
  int addr_hi = (addr >> 8) & 0xff; // 256B blocks
  int block   = (addr >> 12) & 0xf; // 4K blocks

  // 1K RAM (0000-03FF)
  if (addr < 0x400)
    return m_RAM1K[addr];

  // 3K RAM Expansion (0400-0FFF)
  else if (block == 0 && m_expRAM[0])
    return m_expRAM[0][addr - 0x400];

  // 4K RAM (1000-1FFF)
  else if (block == 1)
    return m_RAM4K[addr & 0xFFF];

  // 8K RAM Expansion or Cartridge (2000-3FFF)
  else if (block >= 2 && block <= 3) {
    if (m_expROM[0])
      return m_expROM[0][addr & 0x1fff];
    else if (m_expRAM[1])
      return m_expRAM[1][addr & 0x1fff];
  }

  // 8K RAM expansion or Cartridge (4000-5fff)
  else if (block >= 4 && block <= 5) {
    if (m_expROM[1])
      return m_expROM[1][addr & 0x1fff];
    else if (m_expRAM[2])
      return m_expRAM[2][addr & 0x1fff];
  }

  // 8K RAM expansion or Cartridge (6000-7fff)
  else if (block >= 6 && block <= 7) {
    if (m_expROM[2])
      return m_expROM[2][addr & 0x1fff];
    else if (m_expRAM[3])
      return m_expRAM[3][addr & 0x1fff];
  }

  // 4K ROM (8000-8FFF)
  else if (block == 8)
    return char_rom[addr & 0xfff];

  // VIC (9000-90FF)
  else if (addr_hi == 0x90)
    return m_VIC.readReg(addr & 0xf);

  // VIAs (9100-93FF)
  else if (addr_hi >= 0x91 && addr_hi <= 0x93) {
    if (addr & 0x10)
      return m_VIA1.readReg(addr & 0xf);
    else if (addr & 0x20)
      return m_VIA2.readReg(addr & 0xf);
  }

  // 1Kx4 RAM (9400-97FF)
  else if (addr_hi >= 0x94 && addr_hi <= 0x97)
    return m_RAMColor[addr & 0x3ff] & 0x0f;

  // 8K Cartridge (A000-BFFF)
  else if (block >= 0xa && block <= 0xb) {
    if (m_expROM[3])
      return m_expROM[3][addr & 0x1fff];
    else if (m_expRAM[4])
      return m_expRAM[4][addr & 0x1fff];
  }

  // 8K ROM (C000-DFFF)
  else if (block >= 0xc && block <= 0xd)
    return basic_rom[addr & 0x1fff];

  // 8K ROM (E000-FFFF)
  else if (block >= 0xe && block <= 0xf)
    return kernal_rom[addr & 0x1fff];

  // unwired address returns high byte of the address
  return addr >> 8;
}


void Machine::busWrite(uint16_t addr, uint8_t value)
{
  int addr_hi = (addr >> 8) & 0xff; // 256B blocks
  int block   = (addr >> 12) & 0xf; // 4K blocks

  // 1K RAM (0000-03FF)
  if (addr < 0x400)
    m_RAM1K[addr] = value;

  // 3K RAM Expansion (0400-0FFF)
  else if (m_expRAM[0] && block == 0)
    m_expRAM[0][addr - 0x400] = value;

  // 4K RAM (1000-1FFF)
  else if (block == 1)
    m_RAM4K[addr & 0xFFF] = value;

  // 8K RAM Expansion (2000-3FFF)
  else if (m_expRAM[1] && block >= 2 && block <= 3)
    m_expRAM[1][addr & 0x1fff] = value;

  // 8K RAM expansion (4000-5fff)
  else if (m_expRAM[2] && block >= 4 && block <= 5)
    m_expRAM[2][addr & 0x1fff] = value;

  // 8K RAM expansion (6000-7fff)
  else if (m_expRAM[3] && block >= 6 && block <= 7)
    m_expRAM[3][addr & 0x1fff] = value;

  // VIC (9000-90FF)
  else if (addr_hi == 0x90)
    m_VIC.writeReg(addr & 0xf, value);

  // VIAs (9100-93FF)
  else if (addr_hi >= 0x91 && addr_hi <= 0x93) {
    if (addr & 0x10)
      m_VIA1.writeReg(addr & 0xf, value);
    else if (addr & 0x20)
      m_VIA2.writeReg(addr & 0xf, value);
  }

  // 1Kx4 RAM (9400-97FF)
  else if (addr_hi >= 0x94 && addr_hi <= 0x97)
    m_RAMColor[addr & 0x3ff] = value;

}


// todo: very kbd layout dependant with SDL!! With fabgl should be more natural!
void Machine::setKeyboard(VirtualKey key, bool down)
{
  /*
  switch (key.sym) {
    case SDLK_0:
      if (key.mod & (KMOD_SHIFT)) { // '='
        m_KBD[6][5] = down;
        m_KBD[1][3] = m_KBD[6][4] = !down; // release LSHIFT, RSHIFT
      } else
        m_KBD[4][7] = down;
      break;
    case SDLK_1:
      m_KBD[0][0] = down;
      break;
    case SDLK_2:
      m_KBD[0][7] = down;
      break;
    case SDLK_3:
      if (key.mod & (KMOD_SHIFT)) { // '£'
        m_KBD[6][0] = down;
        m_KBD[1][3] = m_KBD[6][4] = !down; // release LSHIFT, RSHIFT
      } else
        m_KBD[1][0] = down;
      break;
    case SDLK_4:
      m_KBD[1][7] = down;
      break;
    case SDLK_5:
      m_KBD[2][0] = down;
      break;
    case SDLK_6:
      m_KBD[2][7] = down;
      break;
    case SDLK_7:
      if (key.mod & (KMOD_SHIFT)) { // '/'
        m_KBD[6][3] = down;
        m_KBD[1][3] = m_KBD[6][4] = !down; // release LSHIFT, RSHIFT
      } else
        m_KBD[3][0] = down;
      break;
    case SDLK_8:
      m_KBD[3][7] = down;
      break;
    case SDLK_9:
      m_KBD[4][0] = down;
      break;
    case SDLK_w:
      if (key.mod & (KMOD_ALT)) {
        // ALT-W move screen up
        if (down) {
          int c = (int) VIC().readReg(1) - 1;
          if (c < 0) c = 0;
          VIC().writeReg(1, c);
          return;
        }
      }
      m_KBD[1][1] = down;
      break;
    case SDLK_r:
      m_KBD[2][1] = down;
      break;
    case SDLK_y:
      m_KBD[3][1] = down;
      break;
    case SDLK_i:
      m_KBD[4][1] = down;
      break;
    case SDLK_p:
      m_KBD[5][1] = down;
      break;
    case SDLK_a:
      if (key.mod & (KMOD_ALT)) {
        // ALT-A move screen left
        if (down) {
          int c = (int)(VIC().readReg(0) & 0x7f) - 1;
          if (c < 0) c = 0;
          VIC().writeReg(0, c);
          return;
        }
      }
      m_KBD[1][2] = down;
      break;
    case SDLK_d:
      m_KBD[2][2] = down;
      break;
    case SDLK_g:
      m_KBD[3][2] = down;
      break;
    case SDLK_j:
      m_KBD[4][2] = down;
      break;
    case SDLK_l:
      m_KBD[5][2] = down;
      break;
    case SDLK_x:
      m_KBD[2][3] = down;
      break;
    case SDLK_v:
      m_KBD[3][3] = down;
      break;
    case SDLK_n:
      m_KBD[4][3] = down;
      break;
    case SDLK_z:
      if (key.mod & (KMOD_ALT)) {
        // ALT-Z move screen down
        if (down) {
          int c = (int) VIC().readReg(1) + 1;
          if (c > 255) c = 255;
          VIC().writeReg(1, c);
          return;
        }
      }
      m_KBD[1][4] = down;
      break;
    case SDLK_c:
      m_KBD[2][4] = down;
      break;
    case SDLK_b:
      m_KBD[3][4] = down;
      break;
    case SDLK_m:
      m_KBD[4][4] = down;
      break;
    case SDLK_s:
      if (key.mod & (KMOD_ALT)) {
        // ALT-S move screen right
        if (down) {
          int c = (int)(VIC().readReg(0) & 0x7f) + 1;
          if (c == 128) c = 127;
          VIC().writeReg(0, c);
          return;
        }
      }
      m_KBD[1][5] = down;
      break;
    case SDLK_f:
      m_KBD[2][5] = down;
      break;
    case SDLK_h:
      m_KBD[3][5] = down;
      break;
    case SDLK_k:
      m_KBD[4][5] = down;
      break;
    case SDLK_q:
      m_KBD[0][6] = down;
      break;
    case SDLK_e:
      m_KBD[1][6] = down;
      break;
    case SDLK_t:
      m_KBD[2][6] = down;
      break;
    case SDLK_u:
      m_KBD[3][6] = down;
      break;
    case SDLK_o:
      m_KBD[4][6] = down;
      break;
    case 242:
      if (key.mod & (KMOD_ALT)) // '@'
        m_KBD[5][6] = down;
      break;
    case 236:
      if (key.mod & (KMOD_SHIFT)) {
        // '^' => UP ARROW (same ASCII of '^')
        m_KBD[6][6] = down;
        m_KBD[1][3] = m_KBD[6][4] = !down; // release LSHIFT, RSHIFT
      } else {
        // 'ì' => pi
        m_KBD[6][6] = down;
        m_KBD[1][3] = down; // press LSHIFT
      }
      break;
    case SDLK_COMMA:
      if (key.mod & (KMOD_SHIFT)) {
        // ';'
        m_KBD[6][2] = down;
        m_KBD[1][3] = m_KBD[6][4] = !down; // release LSHIFT, RSHIFT
      } else {
        // ','
        m_KBD[5][3] = down;
      }
      break;
    case SDLK_SPACE:
      m_KBD[0][4] = down;
      break;
    case SDLK_MINUS:
      if (key.mod & (KMOD_SHIFT)) {
        // SHIFT '-' => LEFT-ARROW (same ASCII of UNDERSCORE)
        m_KBD[0][1] = down;
        m_KBD[1][3] = m_KBD[6][4] = !down; // release LSHIFT, RSHIFT
      } else {
        // '-'
        m_KBD[5][7] = down;
      }
      break;
    case 232:
      if (key.mod & (KMOD_ALT)) {
        // '['
        m_KBD[5][5] = down;
        m_KBD[1][3] = down; // press LSHIFT
      }
      break;
    case 60:  // '\' => '£'
      m_KBD[6][0] = down;
      break;
    case SDLK_PLUS:
      if (key.mod & (KMOD_SHIFT)) {
        // SHIFT '+' => '*'
        m_KBD[6][1] = down;
        m_KBD[1][3] = m_KBD[6][4] = !down; // release LSHIFT, RSHIFT
      } else if (key.mod & (KMOD_ALT)) {
        // ALT '+' => ']'
        m_KBD[6][2] = down;
        m_KBD[1][3] = down; // press LSHIFT
      } else {
        // '+'
        m_KBD[5][0] = down;
      }
      break;
    case SDLK_BACKSPACE:
      m_KBD[7][0] = down;
      break;
    case 92:
      if (key.mod & (KMOD_SHIFT)) {
        // SHIFT '<' => '>'
        m_KBD[5][4] = down;
        //m_KBD[1][3] = down; // press LSHIFT
      } else {
        // '<'
        m_KBD[4][3] = down;
        m_KBD[1][3] = down; // press LSHIFT
      }
      break;
    case 39:
      if (key.mod & (KMOD_SHIFT)) {
        // SHIFT ''' => '?'
        m_KBD[6][3] = down;
        //m_KBD[1][3] = down; // press LSHIFT
      }
      break;
    case SDLK_PERIOD:
      if (key.mod & (KMOD_SHIFT)) {
        // SHIFT '.' => ':'
        m_KBD[5][5] = down;
        m_KBD[1][3] = m_KBD[6][4] = !down; // release LSHIFT, RSHIFT
      } else {
        // '.'
        m_KBD[5][4] = down;
      }
      break;
    case SDLK_RETURN:
      m_KBD[7][1] = down;
      break;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
      m_KBD[0][2] = down;
      break;
    case SDLK_HOME:
      // HOME
      m_KBD[6][7] = down;
      break;
    case SDLK_ESCAPE:
      // ESC => RUNSTOP
      m_KBD[0][3] = down;
      break;
    case SDLK_LSHIFT:
      m_KBD[1][3] = down;
      break;
    case SDLK_LGUI:
      // LGUI => CBM
      m_KBD[0][5] = down;
      break;
    case SDLK_RSHIFT:
      m_KBD[6][4] = down;
      break;

    case SDLK_LEFT:
      if (key.mod & (KMOD_ALT)) {
        // ALT-LEFT move joystick left
        setJoy(JoyLeft, down);
        return;
      }
      m_KBD[7][2] = down;
      m_KBD[1][3] = down; // press LSHIFT
      break;

    case SDLK_RIGHT:
      if (key.mod & (KMOD_ALT)) {
        // ALT-RIGHT move joystick right
        setJoy(JoyRight, down);
        return;
      }
      m_KBD[7][2] = down;
      break;

    case SDLK_UP:
      if (key.mod & (KMOD_ALT)) {
        // ALT-UP move joystick up
        setJoy(JoyUp, down);
        return;
      }
      m_KBD[7][3] = down;
      m_KBD[1][3] = down; // press LSHIFT
      break;

    case SDLK_DOWN:
      if (key.mod & (KMOD_ALT)) {
        // ALT-DOWN move joystick down
        setJoy(JoyDown, down);
        return;
      }
      m_KBD[7][3] = down;
      break;

    case 249:
      // debug only: 'ù' fire joystick
      setJoy(JoyFire, down);
      break;

    case SDLK_F1:
      m_KBD[7][4] = down;
      break;
    case SDLK_F2:
      m_KBD[7][4] = down;
      m_KBD[1][3] = down; // press LSHIFT
      break;
    case SDLK_F3:
      m_KBD[7][5] = down;
      break;
    case SDLK_F4:
      m_KBD[7][5] = down;
      m_KBD[1][3] = down; // press LSHIFT
      break;
    case SDLK_F5:
      m_KBD[7][6] = down;
      break;
    case SDLK_F6:
      m_KBD[7][6] = down;
      m_KBD[1][3] = down; // press LSHIFT
      break;
    case SDLK_F7:
      m_KBD[7][7] = down;
      break;
    case SDLK_F8:
      m_KBD[7][7] = down;
      m_KBD[1][3] = down; // press LSHIFT
      break;
    case 127:
      // CANC = RESTORE
      VIA1().setCA1(!down);
      break;

    default:
      break;
  }
  */
}


void Machine::VIA1PortOut(MOS6522 * via, VIAPort port)
{
}


void Machine::VIA2PortOut(MOS6522 * via, VIAPort port)
{
}


void Machine::VIA1PortIn(MOS6522 * via, VIAPort port)
{
  Machine * m = via->machine();

  switch (port) {

    // joystick (up, down, left, fire). Right on VIA2:PB
    case Port_PA:
      via->setBitPA(2, !m->m_JOY[JoyUp]);
      via->setBitPA(3, !m->m_JOY[JoyDown]);
      via->setBitPA(4, !m->m_JOY[JoyLeft]);
      via->setBitPA(5, !m->m_JOY[JoyFire]);
      break;

    default:
      break;
  }
}


void Machine::VIA2PortIn(MOS6522 * via, VIAPort port)
{
  Machine * m = via->machine();

  switch (port) {

    // Keyboard Row on PA (input)
    case Port_PA:
    {
      // Keyboard column on PB (output)
      int PA = 0;
      int col = ~(via->PB()) & via->DDRB();
      if (col) {
        for (int c = 0; c < 8; ++c)
          if (col & (1 << c))
            for (int r = 0; r < 8; ++r)
              PA |= (m->m_KBD[r][c] & 1) << r;
      }
      via->setPA(~PA);
      break;
    }

    // PB:7 -> joystick right (this is also used as output for column selection)
    case Port_PB:
    {
      // keyboard can also be queried using PA as output and PB as input
      int row = ~(via->PA()) & via->DDRA();
      if (row) {
        int PB = 0;
        for (int r = 0; r < 8; ++r)
          if (row & (1 << r))
            for (int c = 0; c < 8; ++c)
              PB |= (m->m_KBD[r][c] & 1) << c;
        via->setPB(~PB);
      }
      // joystick
      if ((via->DDRB() & 0x80) == 0)
        via->setBitPB(7, !m->m_JOY[JoyRight]);
      break;
    }

    default:
      break;

  }
}


void Machine::loadPRG(uint8_t const * data, size_t size, bool run)
{
  if (data && size > 2) {
    int loadAddr = data[0] | (data[1] << 8);
    size -= 2;
    for (int i = 0; i < size; ++i)
      busWrite(loadAddr + i, data[2 + i]);

    //// set basic pointers

    // read "Start of Basic"
    int lo = busRead(0x2b);
    int hi = busRead(0x2c);
    int basicStart = lo | (hi << 8);
    int basicEnd   = basicStart + (int)size;

    // "Tape buffer scrolling"
    //busWrite(0xac, lo);
    //busWrite(0xad, hi);
    busWrite(0xac, 0);
    busWrite(0xad, 0);

    lo = basicEnd & 0xff;
    hi = (basicEnd >> 8) & 0xff;

    // "Start of Variables"
    busWrite(0x2d, lo);
    busWrite(0x2e, hi);

    // "Start of Arrays"
    busWrite(0x2f, lo);
    busWrite(0x30, hi);

    // "End of Arrays"
    busWrite(0x31, lo);
    busWrite(0x32, hi);

    // "Tape end addresses/End of program"
    busWrite(0xae, lo);
    busWrite(0xaf, hi);

    if (run)
      type("RUN\r");
  }
}


void Machine::resetJoy()
{
  for (int i = JoyUp; i <= JoyFire; ++i)
    m_JOY[i] = false;
}



/////////////////////////////////////////////////////////////////////////////////////////////
// VIA (6522 - Versatile Interface Adapter)


MOS6522::MOS6522(Machine * machine, int tag, VIAPortIO portOut, VIAPortIO portIn)
  : m_machine(machine),
    m_tag(tag),
    m_portOut(portOut),
    m_portIn(portIn)
{
  reset();
}


void MOS6522::reset()
{
  m_timer1Counter   = 0x0000;
  m_timer1Precount  = 0x0000;
  m_timer1Latch     = 0x0000;
  m_timer2Counter   = 0x0000;
  m_timer2Latch     = 0x00;
  m_CA1             = 0;
  m_CA1_prev        = 0;
  m_CA2             = 0;
  m_CA2_prev        = 0;
  m_CB1             = 0;
  m_CB1_prev        = 0;
  m_CB2             = 0;
  m_CB2_prev        = 0;
  m_IFR             = 0;
  m_IER             = 0;
  m_ACR             = 0;
  m_timer1Triggered = false;
  m_timer2Triggered = false;
  memset(m_regs, 0, 16);
}


#if DEBUGMSG
void MOS6522::dump()
{
  for (int i = 0; i < 16; ++i)
    printf("%02x ", m_regs[i]);
}
#endif


void MOS6522::writeReg(int reg, int value)
{
  #if DEBUGMSG
  printf("VIA %d, writeReg 0x%02x = 0x%02x\n", m_tag, reg, value);
  #endif
  m_regs[reg] = value;
  switch (reg) {
    case VIA_REG_T1_C_LO:
      // timer1: write into low order latch
      m_timer1Latch = (m_timer1Latch & 0xff00) | value;
      break;
    case VIA_REG_T1_C_HI:
      // timer1: write into high order latch
      m_timer1Latch = (m_timer1Latch & 0x00ff) | (value << 8);
      // timer1: write into high order counter
      // timer1: transfer low order latch into low order counter
      m_timer1Counter = (m_timer1Latch & 0x00ff) | (value << 8);
      // clear T1 interrupt flag
      m_IFR &= ~VIA_I_T1;
      m_timer1Triggered = false;
      //m_timer1Precount = 2; // TODO: check
      break;
    case VIA_REG_T1_L_LO:
      // timer1: write low order latch
      m_timer1Latch = (m_timer1Latch & 0xff00) | value;
      break;
    case VIA_REG_T1_L_HI:
      // timer1: write high order latch
      m_timer1Latch = (m_timer1Latch & 0x00ff) | (value << 8);
      // clear T1 interrupt flag
      m_IFR &= ~VIA_I_T1;
      break;
    case VIA_REG_T2_C_LO:
      // timer2: write low order latch
      m_timer2Latch = value;
      break;
    case VIA_REG_T2_C_HI:
      // timer2: write high order counter
      // timer2: copy low order latch into low order counter
      m_timer2Counter = (value << 8) | m_timer2Latch;
      // clear T2 interrupt flag
      m_IFR &= ~VIA_I_T2;
      m_timer2Triggered = false;
      break;
    case VIA_REG_ACR:
      m_ACR = value;
      break;
    case VIA_REG_PCR:
    {
      m_regs[VIA_REG_PCR] = value;
      // CA2 control
      switch ((m_regs[VIA_REG_PCR] >> 1) & 0b111) {
        case 0b110:
          // manual output - low
          m_CA2 = 0;
          m_portOut(this, Port_CA2);
          break;
        case 0b111:
          // manual output - high
          m_CA2 = 1;
          m_portOut(this, Port_CA2);
          break;
        default:
          break;
      }
      // CB2 control
      switch ((m_regs[VIA_REG_PCR] >> 5) & 0b111) {
        case 0b110:
          // manual output - low
          m_CB2 = 0;
          m_portOut(this, Port_CB2);
          break;
        case 0b111:
          // manual output - high
          m_CB2 = 1;
          m_portOut(this, Port_CB2);
          break;
        default:
          break;
      }
      break;
    }
    case VIA_REG_IER:
      // interrupt enable register
      if (value & VIA_I_CTRL) {
        // set 0..6 bits
        m_IER |= value & 0x7f;
      } else {
        // reset 0..6 bits
        m_IER &= ~value & 0x7f;
      }
      break;
    case VIA_REG_IFR:
      // flag register, reset each bit at 1
      m_IER &= ~value;
      break;
    case VIA_REG_DDRA:
      // Direction register Port A
      m_regs[VIA_REG_DDRA] = value;
      break;
    case VIA_REG_DDRB:
      // Direction register Port B
      m_regs[VIA_REG_DDRB] = value;
      break;
    case VIA_REG_ORA:
      // Output on Port A
      m_regs[VIA_REG_ORA] = value | (m_regs[VIA_REG_ORA] & ~m_regs[VIA_REG_DDRA]);
      m_portOut(this, Port_PA);
      // clear CA1 and CA2 interrupt flags
      m_IFR &= ~VIA_I_CA1;
      m_IFR &= ~VIA_I_CA2;
      break;
    case VIA_REG_ORA_NH:
      // Output on Port A (no handshake)
      m_regs[VIA_REG_ORA] = value | (m_regs[VIA_REG_ORA] & ~m_regs[VIA_REG_DDRA]);
      m_portOut(this, Port_PA);
      break;
    case VIA_REG_ORB:
      // Output on Port B
      m_regs[VIA_REG_ORB] = value | (m_regs[VIA_REG_ORB] & ~m_regs[VIA_REG_DDRB]);
      m_portOut(this, Port_PB);
      // clear CB1 and CB2 interrupt flags
      m_IFR &= ~VIA_I_CB1;
      m_IFR &= ~VIA_I_CB2;
      break;
    default:
      break;
  };
}


int MOS6522::readReg(int reg)
{
  #if DEBUGMSG
  printf("VIA %d, readReg 0x%02x\n", m_tag, reg);
  #endif
  switch (reg) {
    case VIA_REG_T1_C_LO:
      // clear T1 interrupt flag
      m_IFR &= ~VIA_I_T1;
      // read T1 low order counter
      return m_timer1Counter & 0xff;
    case VIA_REG_T1_C_HI:
      // read T1 high order counter
      return m_timer1Counter >> 8;
    case VIA_REG_T1_L_LO:
      // read T1 low order latch
      return m_timer1Latch & 0xff;
    case VIA_REG_T1_L_HI:
      // read T1 high order latch
      return m_timer1Latch >> 8;
    case VIA_REG_T2_C_LO:
      // clear T2 interrupt flag
      m_IFR &= ~VIA_I_T2;
      // read T2 low order counter
      return m_timer2Counter & 0xff;
    case VIA_REG_T2_C_HI:
      // read T2 high order counter
      return m_timer2Counter >> 8;
    case VIA_REG_ACR:
      return m_ACR;
    case VIA_REG_PCR:
      return m_regs[VIA_REG_PCR];
    case VIA_REG_IER:
      return m_IER | 0x80;
    case VIA_REG_IFR:
      return m_IFR | (m_IFR & m_IER ? 0x80 : 0);
    case VIA_REG_DDRA:
      // Direction register Port A
      return m_regs[VIA_REG_DDRA];
    case VIA_REG_DDRB:
      // Direction register Port B
      return m_regs[VIA_REG_DDRB];
    case VIA_REG_ORA:
      // clear CA1 and CA2 interrupt flags
      m_IFR &= ~VIA_I_CA1;
      m_IFR &= ~VIA_I_CA2;
      // Input from Port A
      m_portIn(this, Port_PA);
      return m_regs[VIA_REG_ORA];
    case VIA_REG_ORA_NH:
      // Input from Port A (no handshake)
      m_portIn(this, Port_PA);
      return m_regs[VIA_REG_ORA];
    case VIA_REG_ORB:
      // clear CB1 and CB2 interrupt flags
      m_IFR &= ~VIA_I_CB1;
      m_IFR &= ~VIA_I_CB2;
      // Input from Port B
      m_portIn(this, Port_PB);
      return m_regs[VIA_REG_ORB];
    default:
      return m_regs[reg];
  }
}


void MOS6522::tick()
{
  // handle Timer 1
  if (m_timer1Precount > 0) {
    --m_timer1Precount;
  } else {
    --m_timer1Counter;
    if (m_timer1Counter == 0 && !m_timer1Triggered) {
      if (m_ACR & VIA_ACR_T1_FREERUN) {
        // free run, reload from latch
        m_timer1Precount = 2;                   // +2 delay before next start
        m_timer1Counter  = m_timer1Latch;
      } else {
        // one shot
        m_timer1Triggered = true;
      }
      // set interrupt flag
      m_IFR |= VIA_I_T1;
    }
  }

  // handle Timer 2
  if ((m_ACR & VIA_ACR_T2_COUNTPULSES) == 0) {
    --m_timer2Counter;
    if (m_timer2Counter == 0 && !m_timer2Triggered) {
      m_timer2Triggered = true;
      // set interrupt flag
      m_IFR |= VIA_I_T2;
    }
  }

  // handle CA1 (RESTORE key)
  if (m_CA1 != m_CA1_prev) {
    // (interrupt on low->high transition) OR (interrupt on high->low transition)
    if (((m_regs[VIA_REG_PCR] & 1) && m_CA1) || (!(m_regs[VIA_REG_PCR] & 1) && !m_CA1)) {
      m_IFR |= VIA_I_CA1;
    }
    m_CA1_prev = m_CA1;
  }

/*
  // handle CB1
  if (m_CB1 != m_CB1_prev) {
    // (interrupt on low->high transition) OR (interrupt on high->low transition)
    if (((m_regs[VIA_REG_PCR] & 0x10) && m_CB1) || (!(m_regs[VIA_REG_PCR] & 0x10) && !m_CB1)) {
      m_IFR |= VIA_I_CB1;
    }
    m_CB1_prev = m_CB1;
  }
*/

}




/////////////////////////////////////////////////////////////////////////////////////////////
// VIC (6561 - Video Interface Chip)


static const RGB CHARCOLORS[8] = {  {0, 0, 0}, //(0xff << 24) | (0x00 << 16) | (0x00 << 8) | (0x00),   // black
                                    {3, 3, 3}, //    (0xff << 24) | (0xff << 16) | (0xff << 8) | (0xff),   // white
                                    {3, 0, 0}, //   (0xff << 24) | (0xf0 << 16) | (0x00 << 8) | (0x00),   // red
                                    {0, 2, 2}, //   (0xff << 24) | (0x00 << 16) | (0xf0 << 8) | (0xf0),   // cyan
                                    {2, 0, 2}, //    (0xff << 24) | (0x60 << 16) | (0x00 << 8) | (0x60),   // magenta
                                    {0, 2, 0}, //    (0xff << 24) | (0x00 << 16) | (0xa0 << 8) | (0x00),   // green
                                    {0, 0, 2}, //     (0xff << 24) | (0x00 << 16) | (0x00 << 8) | (0xf0),   // blue
                                    {2, 2, 0} }; //    (0xff << 24) | (0xd0 << 16) | (0xd0 << 8) | (0x00) }; // yellow

static const RGB AUXCOLORS[8] = {   {2, 1, 0}, // (0xff << 24) | (0xc0 << 16) | (0xa0 << 8) | (0x00),   // orange
                                    {3, 2, 0}, //    (0xff << 24) | (0xff << 16) | (0xa0 << 8) | (0x00),   // light orange
                                    {3, 2, 2}, //    (0xff << 24) | (0xf0 << 16) | (0x80 << 8) | (0x80),   // pink
                                    {0, 3, 3}, //   (0xff << 24) | (0x00 << 16) | (0xff << 8) | (0xff),   // light cyan
                                    {3, 0, 3}, //    (0xff << 24) | (0xff << 16) | (0x00 << 8) | (0xff),   // light magenta
                                    {0, 3, 0}, //    (0xff << 24) | (0x00 << 16) | (0xff << 8) | (0x00),   // light green
                                    {0, 0, 3}, //    (0xff << 24) | (0x00 << 16) | (0xa0 << 8) | (0xff),   // light blue
                                    {3, 3, 0} };//    (0xff << 24) | (0xff << 16) | (0xff << 8) | (0x00) }; // light yellow



MOS6561::MOS6561(Machine * machine)
  : m_machine(machine)
{
  reset();
}


void MOS6561::reset()
{
  memset(m_regs, 0, 16);
  m_colCount        = 0;
  m_rowCount        = 23;
  m_charHeight      = 8;
  m_videoMatrixAddr = 0x0000;
  m_colorMatrixAddr = 0x0000;
  m_charTableAddr   = 0x0000;
  m_scanX           = 0;
  m_scanY           = 0;
}

void MOS6561::tick()
{
  m_scanX += 4;
  if (m_scanX == FrameWidth) {
    m_scanX = 0;
    ++m_scanY;
    if (m_scanY == FrameHeight)
      m_scanY = 0;
  }
  if (m_scanY >= VerticalBlanking && m_scanX >= HorizontalBlanking)
    drawNextPixels();
}


#define SETPIXEL(x, y, value) VGAController.setRawPixel((x), (y), (value));
//#define SETPIXEL(x, y, value) { VGAController.setRawPixel((x) * 1.6, (y), (value)); VGAController.setRawPixel((x) * 1.6 + 1, (y), (value)); }
/*
#define SETPIXEL(x, y, value) \
{ \
  int pos = (y) * VIC::ScreenWidth + (x); \
  m_frameBuffer[pos] = (value); \
  if (pos < 0 || pos >= VIC::ScreenWidth * VIC::ScreenHeight) \
    printf("setpixel overflow %d\n", pos); \
  else if (y < 0 || y >= VIC::ScreenHeight) \
    printf("setpixel Y overflow %d\n", y); \
  else if (x < 0 || x >= VIC::ScreenWidth) \
    printf("setpixel X overflow %d\n", x); \
}
//*/


// draw next 4 pixels
// TODO: optimize!
void MOS6561::drawNextPixels()
{
  // line to draw relative to frame buffer
  int y = m_scanY - VerticalBlanking;

  // column to draw relative to frame buffer
  int x = m_scanX - HorizontalBlanking;

  int charAreaHeight = m_rowCount * m_charHeight;

  if (y < m_topPos || y >= m_topPos + charAreaHeight || x < m_leftPos || x >= m_leftPos + m_charAreaWidth) {

    //// top/bottom/left/right borders
    SETPIXEL(x + 0, y, m_borderColor);
    SETPIXEL(x + 1, y, m_borderColor);
    SETPIXEL(x + 2, y, m_borderColor);
    SETPIXEL(x + 3, y, m_borderColor);

  } else {

    // chars area

    int charRow = (y - m_topPos) / m_charHeight;
    int charCol = (x - m_leftPos) / CharWidth;

    int charIndex = m_machine->busRead(m_videoMatrixAddr + charRow * m_colCount + charCol);

    // character color code from color RAM
    int foregroundColorCode = m_machine->vicBusRead(m_colorMatrixAddr + charRow * m_colCount + charCol);
    RGB foregroundColor = CHARCOLORS[foregroundColorCode & 7];

    int cy = (y - m_topPos) % m_charHeight;
    int cv = m_machine->vicBusRead(charTableAddress_VIC2CPU(charIndex * m_charHeight + cy));

    int startBit = ((m_leftPos + x) & 0x4) ? 3 : 7;

    if (foregroundColorCode & 0x8) {

      // Multicolor

      int auxColorCode = (m_regs[0xe] >> 4) & 0xf;
      RGB auxColor     = auxColorCode < 8 ? CHARCOLORS[auxColorCode] : AUXCOLORS[auxColorCode & 7];
      const RGB colors[4] = { m_backgroundColor, m_borderColor, foregroundColor, auxColor };

      for (int i = 0; i < 4; i += 2) {
        int code = (cv >> (startBit - 1 - i)) & 0x3;
        SETPIXEL(x + i + 0, y, colors[code]);
        SETPIXEL(x + i + 1, y, colors[code]);
      }

    } else {

      // HI-RES

      RGB bColor = m_backgroundColor;
      RGB fColor = foregroundColor;

      // invert foreground and background colors?
      if (m_invertBKFG) {
        bColor = foregroundColor;
        fColor = m_backgroundColor;
      }

      for (int i = 0; i < 4; ++i)
        SETPIXEL(x + i, y, cv & (1 << (startBit - i)) ? fColor : bColor);

    }

  }
}


void MOS6561::writeReg(int reg, int value)
{
  if (m_regs[reg] != value) {
    m_regs[reg] = value;
    switch (reg) {
      case 0x0:
        m_leftPos = ((m_regs[0] & 0x7f) - 5) * 4;
        break;
      case 0x1:
        m_topPos = (m_regs[1] - 14) * 2;
        break;
      case 0x2:
        m_videoMatrixAddr = ((m_regs[2] & 0x80) << 2) | ((m_regs[5] & 0x70) << 6) | ((~m_regs[5] & 0x80) << 8);
        m_colorMatrixAddr = m_regs[2] & 0x80 ? 0x9600 : 0x9400;
        m_colCount        = m_regs[2] & 0x7f;
        m_charAreaWidth   = m_colCount * CharWidth;
        break;
      case 0x3:
        m_charHeight = m_regs[3] & 1 ? 16 : 8;
        m_rowCount   = (m_regs[3] >> 1) & 0x3f;
        break;
      case 0x5:
        m_charTableAddr   = ((m_regs[5] & 0xf) << 10);
        m_videoMatrixAddr = ((m_regs[2] & 0x80) << 2) | ((m_regs[5] & 0x70) << 6) | ((~m_regs[5] & 0x80) << 8);
        break;
      case 0xf:
      {
        int backColorCode = (m_regs[0xf] >> 4) & 0xf;
        m_backgroundColor = backColorCode < 8 ? CHARCOLORS[backColorCode] : AUXCOLORS[backColorCode & 7];
        m_invertBKFG      = ((m_regs[0xf] & 0x8) == 0);
        m_borderColor     = CHARCOLORS[m_regs[0xf] & 7];
        break;
      }
    }
  }
}


int MOS6561::readReg(int reg)
{
  switch (reg) {
    case 0x3:
      m_regs[0x3] = (m_regs[0x3] & 0x7f) | ((m_scanY & 1) << 7);
      break;
    case 0x4:
      m_regs[0x4] = (m_scanY >> 1) & 0xff;
      break;
  }
  #if DEBUGMSG
  printf("VIC, read reg 0x%02x, val = 0x%02x\n", reg, m_regs[reg]);
  #endif
  return m_regs[reg];
}


// converts VIC char table address to CPU address
// this produces (m_charTableAddr+addr) with correct wrappings at 0x9C00 and 0x1C00
int MOS6561::charTableAddress_VIC2CPU(int addr)
{
  int vaddr = addr + m_charTableAddr;
  return (vaddr & 0x1fff) | (~((vaddr & 0x2000) << 2) & 0x8000);
}



