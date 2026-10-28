#!/usr/bin/env python3
"""Measure one native multiplayer encounter using separate test-engine processes."""

import argparse
import hashlib
import json
import os
import platform
import shutil
import socket
import subprocess
import time
from pathlib import Path


def reserve_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def kernel_queues(port):
    """Sample Linux TCP queues, counting each local endpoint once, not traffic totals."""
    path = Path('/proc/net/tcp')
    if not path.exists():
        return None
    sent = received = connections = 0
    for line in path.read_text().splitlines()[1:]:
        fields = line.split()
        local = int(fields[1].rsplit(':', 1)[1], 16)
        remote = int(fields[2].rsplit(':', 1)[1], 16)
        if port not in (local, remote) or fields[3] == '0A':
            continue
        tx, rx = fields[4].split(':')
        sent += int(tx, 16)
        received += int(rx, 16)
        connections += 1
    return {'txBytes': sent, 'rxBytes': received, 'endpoints': connections}


def run_case(build, output, *, players, seed, difficulty, level, composition,
             character_level, max_ticks=18000, tick_rate=255, classic=False,
             targets=32, targets_file=None, leave_after_kills=0, leavers=0,
             friendly_fire=False, soak_seconds=0, leave_every=0, join_spacing=0,
             timeout=300):
    build, output = Path(build).resolve(), Path(output).resolve()
    worker = build / 'balance_measurement_worker'
    if not worker.is_file():
        raise FileNotFoundError(f'Build the balance_measurement_worker target first: {worker}')
    output.mkdir(parents=True, exist_ok=False)
    if targets_file is not None:
        shutil.copyfile(targets_file, output / 'targets')
    port = reserve_port()
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
               XDG_DATA_HOME=str(output / 'xdg-data'),
               XDG_CONFIG_HOME=str(output / 'xdg-config'))
    processes, logs, commands, samples, history, leaves = [], [], [], [], [], []
    started = time.monotonic()
    failure = None

    def spawn(role, incarnation):
        command = [str(worker), str(output), str(role), str(port), str(players),
                   str(seed), str(difficulty), str(level), composition,
                   str(character_level), str(max_ticks), str(tick_rate),
                   'classic' if classic else 'extended', str(leave_after_kills),
                   str(leavers), str(targets), str(int(friendly_fire)), str(soak_seconds)]
        if leave_every:
            command += [str(leave_every), str(incarnation)]
        commands.append(command)
        log = (output / (f'peer-{role}.log' if incarnation == 0 else f'peer-{role}-{incarnation}.log')).open('w')
        logs.append(log)
        process = subprocess.Popen(command, cwd=build, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        if role < len(processes):
            processes[role] = process
        else:
            processes.append(process)
        history.append({'role': role, 'incarnation': incarnation, 'started': time.monotonic() - started})

    def await_marker(role, marker):
        deadline = min(started + timeout, time.monotonic() + 65)
        while not marker.exists():
            if any(p.poll() is not None and p.returncode != 0 for p in processes):
                raise RuntimeError('An engine failed before admitting the next participant')
            if processes[role].poll() is not None:
                raise RuntimeError(f'Engine {role} exited before it was admitted')
            if time.monotonic() >= deadline:
                raise TimeoutError('Admission did not complete within its deadline')
            time.sleep(.02)

    try:
        for role in range(players):
            if role != 0:
                time.sleep(join_spacing)
            spawn(role, 0)
            await_marker(role, output / ('created' if role == 0 else f'joined-{role}'))
        incarnations = [0] * players
        while any(p.poll() is None for p in processes):
            # Endurance departures: the worker marks left-K with its slot and exits cleanly in town,
            # then a new process takes the slot and rejoins wherever the party is.
            for marker in sorted(output.glob('left-*')):
                departure = int(marker.name.split('-')[1])
                if departure in leaves:
                    continue
                role = int(marker.read_text().strip())
                if processes[role].poll() is None:
                    continue
                if processes[role].returncode != 0:
                    raise RuntimeError(f'Departing engine {role} exited unsuccessfully')
                leaves.append(departure)
                # Markers of the previous occupant must not stand for the new process.
                for stale in (f'joined-{role}', f'closed-{role}', f'result-{role}.json'):
                    (output / stale).unlink(missing_ok=True)
                incarnations[role] += 1
                spawn(role, incarnations[role])
                await_marker(role, output / f'joined-{role}')
                (output / f'rejoined-{departure}').write_text(str(role))
            if any(p.poll() is not None and p.returncode != 0 for p in processes):
                raise RuntimeError('An engine exited unsuccessfully')
            if time.monotonic() - started > timeout:
                raise TimeoutError('Measurement exceeded its wall-clock deadline')
            sample = kernel_queues(port)
            if sample is not None:
                sample['seconds'] = time.monotonic() - started
                samples.append(sample)
            time.sleep(.2)
    except (OSError, RuntimeError, TimeoutError) as error:
        failure = str(error)
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        for log in logs:
            log.close()
    results, manifests = [], []
    for role in range(players):
        result = output / f'result-{role}.json'
        manifest = output / f'hero-{role}.json'
        results.append(json.loads(result.read_text()) if result.exists() else None)
        manifests.append(json.loads(manifest.read_text()) if manifest.exists() else None)
    if len(processes) != players or any(p.returncode != 0 for p in processes) or any(r is None for r in results):
        failure = failure or 'Not every engine produced a successful measurement'
    summary = {
        'scenario': {'players': players, 'seed': seed, 'difficulty': difficulty,
                     'level': level, 'composition': composition,
                     'characterLevel': character_level, 'maxTicks': max_ticks,
                     'tickRate': tick_rate, 'classic': classic, 'targets': targets,
                     'leaveAfterKills': leave_after_kills, 'leavers': leavers,
                     'friendlyFire': friendly_fire, 'soakSeconds': soak_seconds,
                     'leaveEverySeconds': leave_every, 'joinSpacingSeconds': join_spacing},
        'commands': commands, 'codes': [p.returncode for p in processes],
        'history': history, 'leaves': sorted(leaves),
        'wallSeconds': time.monotonic() - started, 'failure': failure,
        'workerSha256': hashlib.sha256(worker.read_bytes()).hexdigest(),
        'platform': platform.platform(), 'results': results, 'heroes': manifests,
        'kernelQueues': samples,
        'queueScope': 'Linux loopback TCP endpoint queues; excludes userspace queues and is not total traffic',
    }
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New directory; existing evidence is never overwritten')
    parser.add_argument('--players', type=int, choices=range(1, 9), default=4)
    parser.add_argument('--seed', type=int, default=20001)
    parser.add_argument('--difficulty', type=int, choices=range(3), default=0)
    parser.add_argument('--level', type=int, choices=range(1, 17), default=2)
    parser.add_argument('--composition', default='W', help='Repeated native classes: W, R, S, or WRS')
    parser.add_argument('--character-level', type=int, choices=range(1, 51), default=8)
    parser.add_argument('--max-ticks', type=int, default=18000)
    parser.add_argument('--tick-rate', type=int, choices=range(1, 256), default=255)
    parser.add_argument('--targets', type=int, default=32, help='Nearest reachable targets; zero selects the entire level')
    parser.add_argument('--targets-file', type=Path, help='Reuse a baseline encounter target manifest')
    parser.add_argument('--classic', action='store_true')
    parser.add_argument('--friendly-fire', action='store_true')
    parser.add_argument('--leave-after-kills', type=int, default=0)
    parser.add_argument('--leavers', type=int, default=0)
    parser.add_argument('--soak-seconds', type=int, default=0,
                        help='Endurance mode: keep cycling town and dungeon levels for this long and append samples-N.jsonl')
    parser.add_argument('--leave-every', type=int, default=0,
                        help='Endurance mode: every this many seconds one of the last three slots leaves in town and a new process rejoins')
    parser.add_argument('--join-spacing', type=float, default=0,
                        help='Seconds between initial joins, like people joining one after another')
    parser.add_argument('--timeout', type=float, default=300)
    args = parser.parse_args()
    if not args.composition or any(c not in 'WRS' for c in args.composition):
        parser.error('Composition must contain only W, R and S')
    if args.classic and args.players > 4:
        parser.error('Classic supports at most four participants')
    if not 0 <= args.leavers < args.players or args.max_ticks <= 0 or args.targets < 0 or args.timeout <= 0 or args.soak_seconds < 0:
        parser.error('Invalid departure count, target count or duration')
    if args.leave_every < 0 or args.join_spacing < 0 or (args.leave_every and (not args.soak_seconds or args.players < 2)):
        parser.error('Departures and rejoins need endurance mode and at least two participants')
    summary = run_case(args.build_dir, args.output, players=args.players, seed=args.seed,
                       difficulty=args.difficulty, level=args.level, composition=args.composition,
                       character_level=args.character_level, max_ticks=args.max_ticks,
                       tick_rate=args.tick_rate, classic=args.classic, targets=args.targets,
                       targets_file=args.targets_file, leave_after_kills=args.leave_after_kills,
                       leavers=args.leavers, friendly_fire=args.friendly_fire,
                       soak_seconds=args.soak_seconds, leave_every=args.leave_every,
                       join_spacing=args.join_spacing, timeout=args.timeout)
    print(json.dumps({'codes': summary['codes'], 'wallSeconds': summary['wallSeconds'],
                      'failure': summary['failure'],
                      'outcomes': [r and r['outcome'] for r in summary['results']],
                      'kills': [r and r['kills'] for r in summary['results']]}))
    return 1 if summary['failure'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
