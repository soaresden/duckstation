#include "sio.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "sio_connection.h"
#include "system.h"
#include "timing_event.h"
Log_SetChannel(SIO);

SIO g_sio;

SIO::SIO() = default;

SIO::~SIO() = default;

void SIO::Initialize()
{
  m_transfer_event = TimingEvents::CreateTimingEvent(
    "SIO Transfer", 1, 1, [](void* param, TickCount ticks, TickCount ticks_late) { g_sio.TransferEvent(); }, nullptr,
    false);

  if (true)
    // m_connection = SIOConnection::CreateSocketServer("0.0.0.0", 1337);
    m_connection = SIOConnection::CreateSocketClient("127.0.0.1", 1337);

  m_stat.bits = 0;
  Reset();
}

void SIO::Shutdown()
{
  m_connection.reset();
  m_transfer_event.reset();
}

void SIO::Reset()
{
  SoftReset();
}

bool SIO::DoState(StateWrapper& sw)
{
  sw.Do(&m_ctrl.bits);
  sw.Do(&m_stat.bits);
  sw.Do(&m_mode.bits);
  sw.Do(&m_baud_rate);

  return !sw.HasError();
}

void SIO::SoftReset()
{
  m_ctrl.bits = 0;
  m_stat.RXPARITY = false;
  m_stat.RXFIFOOVERRUN = false;
  m_stat.RXBADSTOPBIT = false;
  m_stat.INTR = false;
  m_mode.bits = 0;
  m_baud_rate = 0xDC;
  m_data_in.Clear();
  m_data_out = 0;
  m_data_out_full = false;

  UpdateEvent();
  UpdateTXRX();
}

void SIO::UpdateTXRX()
{
  m_stat.TXRDY = m_stat.CTSINPUTLEVEL && !m_data_out_full;
  m_stat.TXDONE = m_ctrl.TXEN && m_stat.TXRDY;
  m_stat.RXFIFONEMPTY = !m_data_in.IsEmpty();
}

void SIO::SetInterrupt()
{
  Log_DevPrintf("Set SIO IRQ");
  m_stat.INTR = true;
  g_interrupt_controller.InterruptRequest(InterruptController::IRQ::SIO);
}

u32 SIO::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00: // SIO_DATA
    {
      m_transfer_event->InvokeEarly(false);

      const u32 data_in_size = m_data_in.GetSize();
      u32 res = 0;
      switch (data_in_size)
      {
        case 7:
        case 6:
        case 5:
        case 4:
          res = ZeroExtend32(m_data_in.Peek(3)) << 24;
          [[fallthrough]];

        case 3:
          res |= ZeroExtend32(m_data_in.Peek(2)) << 16;
          [[fallthrough]];

        case 2:
          res |= ZeroExtend32(m_data_in.Peek(1)) << 16;
          [[fallthrough]];

        case 1:
          res |= ZeroExtend32(m_data_in.Peek(0)) << 16;
          break;

        case 0:
        default:
          res = 0xFFFFFFFFu;
          break;
      }

      Log_DevPrintf("Read SIO_DATA -> 0x%08X", res);
      UpdateTXRX();
      return res;
    }

    case 0x04: // SIO_STAT
    {
      m_transfer_event->InvokeEarly(false);

      const u32 bits = m_stat.bits;
      Log_DevPrintf("Read SIO_STAT -> 0x%08X", bits);
      return bits;
    }

    case 0x08: // SIO_MODE
      return ZeroExtend32(m_mode.bits);

    case 0x0A: // SIO_CTRL
      return ZeroExtend32(m_ctrl.bits);

    case 0x0E: // SIO_BAUD
      return ZeroExtend32(m_baud_rate);

    default:
      Log_ErrorPrintf("Unknown register read: 0x%X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void SIO::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: // SIO_DATA
    {
      Log_DevPrintf("SIO_DATA (W) <- 0x%02X", value);
      m_transfer_event->InvokeEarly(false);

      if (m_data_out_full)
        Log_WarningPrintf("SIO TX buffer overflow, lost 0x%02X when writing 0x%02X", m_data_out, value);

      m_data_out = Truncate8(value);
      m_data_out_full = true;
      UpdateTXRX();
      return;
    }

    case 0x0A: // SIO_CTRL
    {
      Log_DevPrintf("SIO_CTRL <- 0x%04X", value);
      m_transfer_event->InvokeEarly(false);

      m_ctrl.bits = Truncate16(value);
      if (m_ctrl.RESET)
        SoftReset();

      if (m_ctrl.ACK)
      {
        m_stat.RXPARITY = false;
        m_stat.RXFIFOOVERRUN = false;
        m_stat.RXBADSTOPBIT = false;
        m_stat.INTR = false;
      }

      if (!m_ctrl.RXEN)
      {
        m_data_in.Clear();
        UpdateTXRX();
      }
      if (!m_ctrl.TXEN)
      {
        m_data_out_full = false;
        UpdateTXRX();
      }

      return;
    }

    case 0x08: // SIO_MODE
    {
      Log_DevPrintf("SIO_MODE <- 0x%08X", value);
      m_mode.bits = Truncate16(value);
      return;
    }

    case 0x0E:
    {
      Log_DevPrintf("SIO_BAUD <- 0x%08X", value);
      m_baud_rate = Truncate16(value);
      return;
    }

    default:
      Log_ErrorPrintf("Unknown register write: 0x%X <- 0x%08X", offset, value);
      return;
  }
}

TickCount SIO::GetTicksBetweenTransfers() const
{
  static constexpr std::array<u32, 4> mul_factors = {{1, 16, 64, 0}};

  const u32 factor = mul_factors[m_mode.reload_factor];
  const u32 ticks = std::max<u32>((m_baud_rate * factor) & ~u32(1), factor);

  return static_cast<TickCount>(ticks);
}

void SIO::UpdateEvent()
{
  if (!m_connection)
  {
    m_transfer_event->Deactivate();
    return;
  }

  TickCount ticks = GetTicksBetweenTransfers();
  if (ticks == 0)
    ticks = System::GetMaxSliceTicks();

  if (m_transfer_event->GetPeriod() == ticks && m_transfer_event->IsActive())
    return;

  m_transfer_event->Deactivate();
  m_transfer_event->SetPeriodAndSchedule(ticks);
}

void SIO::TransferEvent()
{
  if (m_sync_mode)
    TransferWithSync();
  else
    TransferWithoutSync();
}

void SIO::TransferWithoutSync()
{
  // bytes aren't transmitted when CTS isn't set (i.e. there's nothing on the other side)
  if (m_connection && m_connection->IsConnected())
  {
    m_stat.CTSINPUTLEVEL = true;
    m_stat.DTRINPUTLEVEL = true;

    if (m_ctrl.RXEN)
    {
      u8 data_in;
      u32 data_in_size = m_connection->Read(&data_in, sizeof(data_in), 0);
      if (data_in_size > 0)
      {
        if (m_data_in.IsFull())
        {
          Log_WarningPrintf("FIFO overrun");
          m_data_in.RemoveOne();
          m_stat.RXFIFOOVERRUN = true;
        }

        m_data_in.Push(data_in);

        if (m_ctrl.RXINTEN)
          SetInterrupt();
      }
    }

    if (m_ctrl.TXEN && m_data_out_full)
    {
      const u8 data_out = m_data_out;
      m_data_out_full = false;

      const u32 data_sent = m_connection->Write(&data_out, sizeof(data_out));
      if (data_sent != sizeof(data_out))
        Log_WarningPrintf("Failed to send 0x%02X to connection", data_out);

      if (m_ctrl.TXINTEN)
        SetInterrupt();
    }
  }
  else
  {
    m_stat.CTSINPUTLEVEL = false;
    m_stat.DTRINPUTLEVEL = false;
  }

  UpdateTXRX();
}

void SIO::TransferWithSync()
{
  enum : u8
  {
    STATE_HAS_DATA = (1 << 0),
    STATE_DTR_LEVEL = (1 << 1),
    STATE_CTS_LEVEL = (1 << 2),
  };

  if (!m_connection || !m_connection->IsConnected())
  {
    m_stat.CTSINPUTLEVEL = false;
    m_stat.DTRINPUTLEVEL = false;
    return;
  }

  u8 buf[2] = {};
  if (m_connection->HasData())
  {
    while (m_connection->Read(buf, sizeof(buf), sizeof(buf)) != 0)
    {
      if (buf[0] & STATE_HAS_DATA)
      {
        Log_InfoPrintf("In: %02X %02X", buf[0], buf[1]);

        if (m_data_in.IsFull())
          m_stat.RXFIFOOVERRUN = true;
        else
          m_data_in.Push(buf[1]);

        if (m_ctrl.RXINTEN)
          SetInterrupt();
      }

      if (!m_stat.DTRINPUTLEVEL && buf[0] & STATE_DTR_LEVEL)
        Log_WarningPrintf("DTR active");
      if (!m_stat.CTSINPUTLEVEL && buf[0] & STATE_CTS_LEVEL)
        Log_WarningPrintf("CTS active");

      m_stat.DTRINPUTLEVEL = (buf[0] & STATE_DTR_LEVEL) != 0;
      m_stat.CTSINPUTLEVEL = (buf[0] & STATE_CTS_LEVEL) != 0;
    }
  }

  buf[0] = m_data_in.IsFull() ? 0 : STATE_CTS_LEVEL;
  if (m_ctrl.DTROUTPUT)
    buf[0] |= STATE_DTR_LEVEL;

  buf[1] = 0;
  if (m_data_out_full)
  {
    Log_InfoPrintf("Out: %02X %02X", buf[0], buf[1]);

    buf[0] |= STATE_HAS_DATA;
    buf[1] = m_data_out;
    m_data_out_full = false;

    if (m_ctrl.TXINTEN)
      SetInterrupt();
  }

  if (m_connection->Write(buf, sizeof(buf)) != sizeof(buf))
    Log_WarningPrintf("Write failed");
}