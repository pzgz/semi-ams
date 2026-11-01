#include "fsm.h"

#include <stdarg.h>
#include <stdio.h>

namespace semi {

const char* Fsm::stateName(State s) {
  switch (s) {
    case State::NoMaterial: return "无耗材";
    case State::Loading:    return "耗材吸入中";
    case State::Ready:      return "耗材就绪";
    case State::Printing:   return "打印中";
    case State::Unloading:  return "退料中";
    case State::Retracted:  return "已退料";
    case State::Emergency:  return "紧急停止";
  }
  return "未知状态";
}

const char* Fsm::stateKey(State s) {
  switch (s) {
    case State::NoMaterial: return "NO_MATERIAL";
    case State::Loading:    return "MATERIAL_LOADING";
    case State::Ready:      return "MATERIAL_READY";
    case State::Printing:   return "PRINTING";
    case State::Unloading:  return "UNLOADING";
    case State::Retracted:  return "MATERIAL_RETRACTED";
    case State::Emergency:  return "EMERGENCY_STOPPED";
  }
  return "UNKNOWN_STATE";
}

const char* Fsm::motorName(MotorDir d) {
  switch (d) {
    case MotorDir::Forward: return "正转";
    case MotorDir::Reverse: return "反转";
    default:                return "停止";
  }
}

void Fsm::logf(const char* fmt, ...) {
  if (!log_) return;
  char buf[112];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_(buf);
}

void Fsm::setNote(const char* n) {
  if (!n) n = "";
  if (n != note_) {
    note_ = n;
    rev_++;
  }
}

void Fsm::setMotor(MotorDir d, uint8_t speed) {
  if (d == MotorDir::Stop) speed = 0;
  if (d != motor_.dir || speed != motor_.speed) {
    if (d != motor_.dir) rev_++;
    motor_.dir = d;
    motor_.speed = speed;
  }
}

void Fsm::begin(const Inputs& in, uint32_t now) {
  // 上电不会自动转电机：进料口有耗材但没到就绪位置，按"已退料"处理，等按黑色按钮再吸料
  last_ = in;
  prevNow_ = now;
  enter(in.ready ? State::Ready : (in.in ? State::Retracted : State::NoMaterial), now, "上电");
}

void Fsm::enter(State s, uint32_t now, const char* why) {
  logf("状态: %s -> %s（%s）", stateName(state_), stateName(s), why);
  state_ = s;
  enteredAt_ = now;
  rev_++;
  if (s != State::Emergency) setNote("");

  switch (s) {
    case State::NoMaterial:
      setMotor(MotorDir::Stop, 0);
      armed_ = !last_.in;  // 进料口还有耗材时，要等它空过一次才允许下一次自动吸入
      inTiming_ = false;
      break;
    case State::Loading:
      fullArmed_ = !last_.full;
      setMotor(MotorDir::Forward, params_.loadSpeed);
      break;
    case State::Ready:
    case State::Retracted:
      startAfterLoad_ = false;
      readyBySwitch_ = last_.ready;
      setMotor(MotorDir::Stop, 0);
      break;
    case State::Printing:
      startAfterLoad_ = false;
      // 开始打印时先一直送料直到缓冲器满（耗材要先走完到打印机的 PTFE 管）
      feeding_ = !last_.full;
      feedStart_ = now;
      feedLimit_ = params_.firstFeedTimeoutMs;
      outOfMaterial_ = false;
      refillTiming_ = false;
      break;
    case State::Unloading:
      clearing_ = false;
      readySeen_ = last_.ready;
      if (!readySeen_) logf("就绪开关没有耗材，将一直退到耗材离开进料口");
      setMotor(MotorDir::Reverse, params_.unloadSpeed);
      break;
    case State::Emergency:
      feeding_ = false;
      setMotor(MotorDir::Stop, 0);
      break;
  }
}

void Fsm::emergency(const char* reason, uint32_t now) {
  if (state_ != State::Emergency) resume_ = state_;
  setNote(reason);
  logf("紧急停止：%s", reason);
  enter(State::Emergency, now, reason);
}

void Fsm::forceEmergency(const char* reason, uint32_t now) {
  if (state_ == State::Emergency) {
    setNote(reason);
    return;
  }
  emergency(reason, now);
}

void Fsm::update(const Inputs& in, const Commands& cmd, uint32_t now) {
  last_ = in;
  // 上一个周期电机是否在正转（用来累计料尾送料时间）
  const uint32_t dt = now - prevNow_;
  const bool wasForward = motor_.dir == MotorDir::Forward;
  prevNow_ = now;
  // ---- 红色按钮：任何状态下都优先处理 ----
  if (cmd.emergency) {
    if (state_ == State::Emergency) {
      logf("解除紧急停止，恢复到：%s", stateName(resume_));
      enter(resume_, now, "解除紧急停止");
    } else {
      emergency("手动紧急停止", now);
    }
    return;
  }

  const bool hasMaterial = in.in || in.ready;

  switch (state_) {
    case State::NoMaterial: {
      setMotor(MotorDir::Stop, 0);
      if (!in.in) {
        armed_ = true;
        inTiming_ = false;
      }
      if (in.ready) {
        enter(State::Ready, now, "检测到耗材已就绪");
        break;
      }
      if (in.in && !armed_) {  // 例如紧急停止期间插入了耗材：当作已退料，按黑色按钮吸料
        enter(State::Retracted, now, "进料口有耗材");
        break;
      }
      if (armed_ && in.in) {
        if (!inTiming_) {
          inTiming_ = true;
          inSince_ = now;
        } else if (now - inSince_ >= params_.insertDelayMs) {
          startAfterLoad_ = false;
          enter(State::Loading, now, "耗材插入");
          break;
        }
      }
      if (cmd.startStop) {
        setNote("无耗材，无法开始");
        logf("无耗材，忽略开始命令");
      }
      if (cmd.printerStart) {
        setNote("打印机已开始打印，但没有耗材");
        logf("打印机开始打印，但进料器没有耗材");
      }
      break;
    }

    case State::Loading: {
      setMotor(MotorDir::Forward, params_.loadSpeed);
      if (!in.full) fullArmed_ = true;
      if ((cmd.startStop || cmd.printerStart) && !startAfterLoad_) {
        startAfterLoad_ = true;
        logf("吸料完成后直接开始打印");
      }
      if (in.ready || (fullArmed_ && in.full)) {
        if (!in.ready) logf("就绪开关未触发，改用满料霍尔判断就绪");
        if (startAfterLoad_) enter(State::Printing, now, "吸料完成，开始打印");
        else enter(State::Ready, now, in.ready ? "耗材就绪" : "缓冲器已满");
      } else if (!in.in) {
        enter(State::NoMaterial, now, "耗材抽出");
      } else if (now - enteredAt_ > params_.loadTimeoutMs) {
        emergency("吸入超时", now);
      }
      break;
    }

    case State::Ready: {
      setMotor(MotorDir::Stop, 0);
      if (cmd.startStop) {
        enter(State::Printing, now, "开始打印");
      } else if (cmd.printerStart) {
        enter(State::Printing, now, "打印机开始打印");
      } else if (!hasMaterial) {
        enter(State::NoMaterial, now, "耗材抽出");
      } else if (in.ready) {
        readyBySwitch_ = true;
      } else if (readyBySwitch_) {
        enter(State::Retracted, now, "耗材离开就绪开关");
      }
      break;
    }

    case State::Retracted: {
      setMotor(MotorDir::Stop, 0);
      if (in.ready) {
        enter(State::Ready, now, "检测到耗材已就绪");
      } else if (!in.in) {
        enter(State::NoMaterial, now, "耗材抽出");
      } else if (cmd.startStop || cmd.printerStart) {
        startAfterLoad_ = true;
        enter(State::Loading, now, cmd.startStop ? "重新吸料，完成后开始打印" : "打印机开始打印，先重新吸料");
      }
      break;
    }

    case State::Printing: {
      if (cmd.startStop || cmd.printerUnload) {
        const char* why = cmd.startStop ? "结束打印" : "打印机已退料";
        if (hasMaterial) enter(State::Unloading, now, why);
        else enter(State::NoMaterial, now, cmd.startStop ? "结束打印（无耗材）" : "打印机已退料（无耗材）");
        break;
      }

      // 进料口开关松开 = 耗材用尽。料尾此时还夹在齿轮里，要继续送料把它送出齿轮，
      // 直到料尾过了就绪开关（或送料时间达到 tailFeedMs），之后不再补料
      if (!in.in) {
        if (!outOfMaterial_) {
          outOfMaterial_ = true;
          tailDone_ = params_.tailFeedMs == 0;
          tailReadySeen_ = in.ready;
          tailUsedMs_ = 0;
          setNote("耗材已用尽");
          if (tailDone_) logf("进料口没有耗材，停止补料");
          else logf("进料口没有耗材，继续送料直到料尾离开齿轮");
        }
        refillTiming_ = false;
      } else if (outOfMaterial_) {
        // 打印中换上新耗材：等插入延时后，按"首次送料"重新送到满（要重新走完 PTFE 管）
        if (!refillTiming_) {
          refillTiming_ = true;
          refillSince_ = now;
        } else if (now - refillSince_ >= params_.insertDelayMs) {
          outOfMaterial_ = false;
          refillTiming_ = false;
          setNote("");
          feeding_ = !in.full;
          feedStart_ = now;
          feedLimit_ = params_.firstFeedTimeoutMs;
          logf("检测到新耗材，重新送料到满");
        }
      }
      if (outOfMaterial_) {
        if (!tailDone_) {
          if (wasForward) tailUsedMs_ += dt;
          if (in.ready) tailReadySeen_ = true;
          if (tailReadySeen_ && !in.ready) {
            tailDone_ = true;
            logf("料尾已过就绪开关，停止送料");
          } else if (tailUsedMs_ >= params_.tailFeedMs) {
            tailDone_ = true;
            logf("料尾送料已达 %lums，停止送料", (unsigned long)params_.tailFeedMs);
          }
        }
        if (tailDone_) {
          feeding_ = false;
          setMotor(MotorDir::Stop, 0);
          break;
        }
        // 料尾还没送出：照常按缓冲器送料（到 FULL 暂停，EMPTY 再送），下面的逻辑不变
      }

      if (feeding_) {
        if (in.full) {
          feeding_ = false;
          logf("缓冲器已满，停止补料");
        } else if (now - feedStart_ > feedLimit_) {
          emergency("送料超时", now);
          break;
        }
      } else if (in.lack && !in.full) {
        feeding_ = true;
        feedStart_ = now;
        feedLimit_ = params_.feedTimeoutMs;
        logf("缓冲器已空，开始补料");
      }
      setMotor(feeding_ ? MotorDir::Forward : MotorDir::Stop, params_.feedSpeed);
      break;
    }

    case State::Unloading: {
      setMotor(MotorDir::Reverse, params_.unloadSpeed);
      if (in.ready) readySeen_ = true;
      // 再按一次黑键：取消退料，回到打印中重新送料（要停止电机用红色按钮）
      if (cmd.startStop || cmd.printerStart) {
        const char* why = cmd.startStop ? "取消退料，继续打印" : "打印机开始打印，取消退料";
        if (hasMaterial) enter(State::Printing, now, why);
        else enter(State::NoMaterial, now, "取消退料（无耗材）");
        break;
      }
      // 正常：耗材退过就绪开关后再多退一点，停在进料口里（已退料）
      // 就绪开关从没触发过（开关失效）或耗材已完全退出：退到两个开关都没有耗材
      const bool done = !hasMaterial || (readySeen_ && !in.ready);
      if (done) {
        if (!clearing_) {
          clearing_ = true;
          clearSince_ = now;
        } else if (now - clearSince_ >= params_.unloadExtraMs) {
          enter(in.in ? State::Retracted : State::NoMaterial, now, "退料完成");
          break;
        }
      } else {
        clearing_ = false;
      }
      if (now - enteredAt_ > params_.unloadTimeoutMs) emergency("退料超时", now);
      break;
    }

    case State::Emergency: {
      setMotor(MotorDir::Stop, 0);
      if (cmd.startStop) logf("紧急停止中，请先按红色按钮解除");
      if (cmd.printerStart || cmd.printerUnload) logf("紧急停止中，忽略打印机的%s", cmd.printerStart ? "开始打印" : "退料");
      break;
    }
  }
}

}  // namespace semi
