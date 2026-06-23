using System.Collections.Generic;
using Basis.BasisUI;
using Basis.Scripts.Drivers;
using Basis.Scripts.Networking;
using UnityEngine;
using UnityEngine.UI;

namespace Basis.BasisUI.MediaPlayer
{
    public class BasisMediaPlayerPanelProvider : BasisMenuActionProvider<BasisMainMenu>
    {
        public const string Perm_Control = "basis.mediaplayer.control";
        public const string StaticTitle = "Media Players";

        private static BasisMediaPlayerPanelProvider _instance;

        public override string Title => StaticTitle;
        public override string IconAddress => AddressableAssets.Sprites.Camera;
        public override int Order => 8;
        public override bool Hidden => BasisMediaPlayerRegistry.Count == 0;

        private BasisMenuPanel _panel;
        private RectTransform _scrollContent;
        private PanelDropdown _selector;
        private PanelElementDescriptor _controlGroup;
        private PanelElementDescriptor _userGroup;
        private PanelElementDescriptor _adminGroup;
        private PanelElementDescriptor _emptyState;
        private PanelElementDescriptor _statusGroup;
        private PanelElementDescriptor _debugGroup;
        private PanelToggle _debugToggle;
        private PanelTextField _urlField;
        private PanelSlider _volumeSlider;
        private PanelToggle _captionsToggle;
        private PanelSlider _captionTextOpacitySlider;
        private PanelSlider _captionBgOpacitySlider;
        private PanelDropdown _bitrateDropdown;
        private PanelDropdown _audioTrackDropdown;
        private PanelToggle _advancedToggle;
        private PanelToggle _adminOnlyToggle;
        private PanelToggle _allowAnyoneToggle;
        private PanelSlider _driftSlider;
        private BasisMediaPlayer _activePlayer;
        private BasisMediaPlayerNetworking _activeNetworking;
        private readonly List<BasisMediaPlayer> _entries = new List<BasisMediaPlayer>();
        private bool _panelTickSubscribed;
        private bool _debugMode;
        private string _lastStatusMarkup;
        private BasisMediaPlayerStatus _lastStatus = (BasisMediaPlayerStatus)(-1);
        private Vector2Int _lastStatusSize = new Vector2Int(-1, -1);
        private string _lastStatusErr;
        private readonly System.Text.StringBuilder _debugBuilder = new System.Text.StringBuilder(256);
        private readonly System.Text.StringBuilder _statusBuilder = new System.Text.StringBuilder(192);

        [RuntimeInitializeOnLoadMethod]
        public static void AddToMenu()
        {
            _instance = new BasisMediaPlayerPanelProvider();
            BasisMenuBase<BasisMainMenu>.AddProvider(_instance);
            BasisMediaPlayerRegistry.OnChanged += RefreshMainMenu;
        }

        private static void RefreshMainMenu()
        {
            if (BasisMenuBase<BasisMainMenu>.Instance) BasisMenuBase<BasisMainMenu>.Instance.BindProvidersToButtons();
            if (BasisMainMenu.ActiveMenuTitle == StaticTitle && _instance != null) _instance.RebuildSelector();
        }

        public static bool HasControlPermission()
        {
            if (!BasisNetworkConnection.LocalPlayerIsConnected) return true;
            var perms = BasisNetworkManagement.LocalPermissions;
            return perms != null && (perms.Contains(Perm_Control) || perms.Contains("*"));
        }

        public static bool IsAdmin()
        {
            if (!BasisNetworkConnection.LocalPlayerIsConnected) return true;
            var perms = BasisNetworkManagement.LocalPermissions;
            return perms != null && perms.Contains("*");
        }

        public override void RunAction()
        {
            if (BasisMainMenu.ActiveMenuTitle == Title)
            {
                BasisMainMenu.CloseActivePanel();
                return;
            }

            BasisMenuPanel panel = BasisMainMenu.CreateActiveMenu(
                BasisMenuPanel.PanelData.Standard(Title),
                BasisMenuPanel.PanelStyles.Page);
            BoundButton?.BindActiveStateToAddressablesInstance(panel);
            _panel = panel;

            panel.OnInstanceReleased += OnPanelClosed;

            RectTransform container = panel.Descriptor.ContentParent;
            PanelElementDescriptor scroll = PanelElementDescriptor.CreateNew(
                PanelElementDescriptor.ElementStyles.ScrollViewVertical, container);
            _scrollContent = scroll.ContentParent;

            _selector = PanelDropdown.CreateNewEntry(_scrollContent);
            _selector.Descriptor.SetTitle("Player");
            _selector.OnValueChanged = _ => OnSelectionChanged();

            _emptyState = PanelElementDescriptor.CreateNew(
                PanelElementDescriptor.ElementStyles.Group, _scrollContent);
            _emptyState.SetTitle("No Media Players");
            _emptyState.SetDescription("Spawn or load a prop/scene that contains a Basis Media Player.");

            BuildStatusGroup(_scrollContent);
            BuildControlGroup(_scrollContent);
            BuildUserGroup(_scrollContent);
            BuildAdminGroup(_scrollContent);
            BuildDebugGroup(_scrollContent);

            RebuildSelector();

            // One frame-clock request for the panel's lifetime keeps the Status line
            // live (Connecting → Buffering → Playing/Error are polled, not evented).
            SetPanelTickSubscription(true);
        }

        private void OnPanelClosed()
        {
            SetPanelTickSubscription(false);
            UnsubscribeFromActivePlayer();
            _debugMode = false;
            _lastStatusMarkup = null;
            // Invalidate the status gate so a reopened panel (this provider is a reused
            // singleton) always repaints its fresh "—" status group on first tick.
            _lastStatus = (BasisMediaPlayerStatus)(-1);
            _panel = null;
            _scrollContent = null;
            _selector = null;
            _controlGroup = null;
            _userGroup = null;
            _adminGroup = null;
            _emptyState = null;
            _statusGroup = null;
            _debugGroup = null;
            _debugToggle = null;
            _urlField = null;
            _volumeSlider = null;
            _captionsToggle = null;
            _captionTextOpacitySlider = null;
            _captionBgOpacitySlider = null;
            _bitrateDropdown = null;
            _audioTrackDropdown = null;
            _advancedToggle = null;
            _adminOnlyToggle = null;
            _allowAnyoneToggle = null;
            _driftSlider = null;
            _activePlayer = null;
            _activeNetworking = null;
            _entries.Clear();
        }

        public override void OnReleaseEvent() => OnPanelClosed();

        private void BuildControlGroup(RectTransform parent)
        {
            _controlGroup = PanelElementDescriptor.CreateNew(PanelElementDescriptor.ElementStyles.Group, parent);
            _controlGroup.SetTitle("Playback");
            _controlGroup.SetDescription("Requires the basis.mediaplayer.control permission.");
            RectTransform content = _controlGroup.ContentParent;

            _urlField = PanelTextField.CreateNewEntry(content);
            _urlField.Descriptor.SetTitle("URL");

            RectTransform actions = BuildActionRow(content);

            PanelButton loadBtn = PanelButton.CreateNew(actions);
            loadBtn.Descriptor.SetTitle("Load URL");
            loadBtn.OnClicked += () =>
            {
                if (_activePlayer == null || _urlField == null) return;
                string u = _urlField.Value;
                if (string.IsNullOrEmpty(u)) return;
                if (_activeNetworking != null) _ = _activeNetworking.SetUrl(u);
                else _activePlayer.LoadUrl(u);
            };

            PanelButton playBtn = PanelButton.CreateNew(actions);
            playBtn.Descriptor.SetTitle("Play");
            playBtn.OnClicked += () =>
            {
                if (_activePlayer == null) return;
                if (_activeNetworking != null) _ = _activeNetworking.Play();
                else _activePlayer.Play();
            };

            PanelButton pauseBtn = PanelButton.CreateNew(actions);
            pauseBtn.Descriptor.SetTitle("Pause");
            pauseBtn.OnClicked += () =>
            {
                if (_activePlayer == null) return;
                if (_activeNetworking != null) _ = _activeNetworking.Pause();
                else _activePlayer.Pause();
            };

            PanelButton stopBtn = PanelButton.CreateNew(actions);
            stopBtn.Descriptor.SetTitle("Stop");
            stopBtn.OnClicked += () =>
            {
                if (_activePlayer == null) return;
                if (_activeNetworking != null) _ = _activeNetworking.Stop();
                else _activePlayer.Stop();
            };

            _bitrateDropdown = PanelDropdown.CreateNewEntry(content);
            _bitrateDropdown.Descriptor.SetTitle("Bitrate");
            _bitrateDropdown.OnValueChanged = _ =>
            {
                if (_activePlayer == null || _bitrateDropdown == null) return;
                int idx = _bitrateDropdown.Index;
                if (idx > 0) _activePlayer.SelectBitrate(idx - 1);
            };

            _audioTrackDropdown = PanelDropdown.CreateNewEntry(content);
            _audioTrackDropdown.Descriptor.SetTitle("Audio Track");
            _audioTrackDropdown.OnValueChanged = _ =>
            {
                if (_activePlayer == null || _audioTrackDropdown == null) return;
                int idx = _audioTrackDropdown.Index;
                if (idx >= 0) _activePlayer.SelectAudioTrack(idx);
            };
        }

        private void BuildUserGroup(RectTransform parent)
        {
            _userGroup = PanelElementDescriptor.CreateNew(PanelElementDescriptor.ElementStyles.Group, parent);
            _userGroup.SetTitle("My Settings");
            _userGroup.SetDescription("Client-side controls — only affect your own playback.");
            RectTransform content = _userGroup.ContentParent;

            _volumeSlider = PanelSlider.CreateNew(content);
            _volumeSlider.SetSliderSettings(PanelSlider.SliderSettings.Percentage("Volume"));
            _volumeSlider.OnValueChanged = v =>
            {
                if (_activePlayer == null) return;
                float volume = Mathf.Clamp01(v / 100f);
                _activePlayer.Volume = volume;
                _activePlayer.Mute = volume <= 0f;
                if (_activePlayer.AudioComponent != null)
                {
                    _activePlayer.AudioComponent.VolumeGain = volume;
                    _activePlayer.AudioComponent.Mute = volume <= 0f;
                }
            };

            _captionsToggle = PanelToggle.CreateNewEntry(content);
            _captionsToggle.Descriptor.SetTitle("Captions (CC)");
            _captionsToggle.Descriptor.SetDescription("Show in-band closed captions when the stream carries them.");
            _captionsToggle.OnValueChanged = v =>
            {
                if (_activePlayer != null) _activePlayer.CaptionsEnabled = v;
                ApplyCaptionOptionsVisibility(v);
            };

            _captionTextOpacitySlider = PanelSlider.CreateNew(content);
            _captionTextOpacitySlider.SetSliderSettings(PanelSlider.SliderSettings.Percentage("Text Opacity"));
            _captionTextOpacitySlider.OnValueChanged = v =>
            {
                if (_activePlayer != null) _activePlayer.CaptionTextOpacity = Mathf.Clamp01(v / 100f);
            };

            _captionBgOpacitySlider = PanelSlider.CreateNew(content);
            _captionBgOpacitySlider.SetSliderSettings(PanelSlider.SliderSettings.Percentage("Background Opacity"));
            _captionBgOpacitySlider.OnValueChanged = v =>
            {
                if (_activePlayer != null) _activePlayer.CaptionBackgroundOpacity = Mathf.Clamp01(v / 100f);
            };

            RectTransform actions = BuildActionRow(content);
            PanelButton resyncBtn = PanelButton.CreateNew(actions);
            resyncBtn.Descriptor.SetTitle("Resync");
            resyncBtn.OnClicked += () =>
            {
                if (_activePlayer == null) return;
                _activePlayer.Reload();
            };

            _advancedToggle = PanelToggle.CreateNewEntry(content);
            _advancedToggle.Descriptor.SetTitle("Advanced");
            _advancedToggle.OnValueChanged = v =>
            {
                ApplyAdvancedVisibility(v);
            };
        }

        private void RebuildSelector()
        {
            if (_selector == null) return;

            _entries.Clear();
            List<string> labels = new List<string>();
            for (int i = 0; i < BasisMediaPlayerRegistry.Players.Count; i++)
            {
                BasisMediaPlayer p = BasisMediaPlayerRegistry.Players[i];
                if (p == null) continue;
                _entries.Add(p);
                string name = !string.IsNullOrEmpty(p.DisplayName)
                    ? p.DisplayName
                    : (p.gameObject != null ? p.gameObject.name : "(destroyed)");
                labels.Add($"{i + 1}. {name}");
            }

            _selector.AssignEntries(labels);

            if (_entries.Count == 0)
            {
                _selector.gameObject.SetActive(false);
                _emptyState?.SetActive(true);
                _statusGroup?.SetActive(false);
                SetGroupsActive(false);
                _activePlayer = null;
                return;
            }

            _selector.gameObject.SetActive(true);
            _emptyState?.SetActive(false);
            _statusGroup?.SetActive(true);

            int idx = _activePlayer != null ? _entries.IndexOf(_activePlayer) : 0;
            if (idx < 0) idx = 0;
            UnsubscribeFromActivePlayer();
            _activePlayer = _entries[idx];
            SubscribeToActivePlayer();
            _selector.SetValueWithoutNotify(labels[idx]);

            ApplyActivePlayerToControls();
        }

        private void OnSelectionChanged()
        {
            if (_selector == null) return;
            int idx = _selector.Index;
            if (idx < 0 || idx >= _entries.Count) return;
            UnsubscribeFromActivePlayer();
            _activePlayer = _entries[idx];
            SubscribeToActivePlayer();
            ApplyActivePlayerToControls();
        }

        private void SubscribeToActivePlayer()
        {
            if (_activePlayer == null) return;
            _activePlayer.OnBitrateTrackChanged += HandleActiveBitrateChanged;
            _activePlayer.OnAudioTrackChanged += HandleActiveAudioTrackChanged;
        }

        private void UnsubscribeFromActivePlayer()
        {
            if (_activePlayer == null) return;
            _activePlayer.OnBitrateTrackChanged -= HandleActiveBitrateChanged;
            _activePlayer.OnAudioTrackChanged -= HandleActiveAudioTrackChanged;
        }

        private void HandleActiveBitrateChanged(BasisBitrateTrack _) => RebuildBitrateDropdown();
        private void HandleActiveAudioTrackChanged(BasisAudioTrack _) => RebuildAudioTrackDropdown();

        private void ApplyActivePlayerToControls()
        {
            bool canControl = HasControlPermission();
            _controlGroup?.SetActive(canControl);
            _userGroup?.SetActive(true);

            _activeNetworking = null;
            if (_activePlayer != null)
            {
                _activePlayer.TryGetComponent(out _activeNetworking);
            }

            bool showAdmin = IsAdmin() && _activeNetworking != null;
            if (_adminGroup != null && _adminGroup.gameObject.activeSelf != showAdmin)
            {
                _adminGroup.gameObject.SetActive(showAdmin);
                _adminGroup.ForceRebuild();
                _panel?.Descriptor?.ForceRebuild();
            }

            if (showAdmin)
            {
                _adminOnlyToggle?.SetValueWithoutNotify(_activeNetworking.AdminOnly);
                _allowAnyoneToggle?.SetValueWithoutNotify(_activeNetworking.AllowAnyoneToTakeControl);
                _driftSlider?.SetValueWithoutNotify(_activeNetworking.DriftSeekThresholdSeconds);
            }

            if (_activePlayer == null) return;

            if (canControl && _urlField != null)
            {
                string current = _activeNetworking != null
                    ? _activeNetworking.SyncedUrl
                    : (_activePlayer.ActiveMediaSource != null ? _activePlayer.ActiveMediaSource.Uri : string.Empty);
                _urlField.SetValueWithoutNotify(current ?? string.Empty);
            }

            _volumeSlider?.SetValueWithoutNotify(_activePlayer.Mute ? 0f : Mathf.Clamp01(_activePlayer.Volume) * 100f);
            _captionsToggle?.SetValueWithoutNotify(_activePlayer.CaptionsEnabled);
            _captionTextOpacitySlider?.SetValueWithoutNotify(Mathf.Clamp01(_activePlayer.CaptionTextOpacity) * 100f);
            _captionBgOpacitySlider?.SetValueWithoutNotify(Mathf.Clamp01(_activePlayer.CaptionBackgroundOpacity) * 100f);
            ApplyCaptionOptionsVisibility(_activePlayer.CaptionsEnabled);

            RebuildBitrateDropdown();
            RebuildAudioTrackDropdown();

            if (_debugToggle != null) _debugToggle.SetValueWithoutNotify(_activePlayer.VerboseLogging);
            RefreshStatus();
            if (_debugMode) RefreshDebugInfo();
        }

        private void RebuildBitrateDropdown()
        {
            if (_bitrateDropdown == null || _activePlayer == null) return;
            var tracks = _activePlayer.BitrateTracks;
            var labels = new List<string>();
            labels.Add("Auto");
            for (int i = 0; i < tracks.Count; i++)
            {
                var t = tracks[i];
                string mbps = t.BitsPerSecond > 0 ? $"{t.BitsPerSecond / 1_000_000f:0.0} Mbps" : "?";
                string dims = t.Height > 0 ? $"{t.Width}x{t.Height}" : "audio";
                string label = !string.IsNullOrEmpty(t.Label) ? t.Label : $"{dims} @ {mbps}";
                labels.Add(label);
            }
            _bitrateDropdown.AssignEntries(labels);
            int sel = _activePlayer.SelectedBitrateIndex;
            int row = sel >= 0 && sel < tracks.Count ? sel + 1 : 0;
            if (row < labels.Count) _bitrateDropdown.SetValueWithoutNotify(labels[row]);
            _bitrateDropdown.gameObject.SetActive(tracks.Count > 0);
        }

        private void RebuildAudioTrackDropdown()
        {
            if (_audioTrackDropdown == null || _activePlayer == null) return;
            var tracks = _activePlayer.AudioTracks;
            var labels = new List<string>();
            for (int i = 0; i < tracks.Count; i++)
            {
                var t = tracks[i];
                string lang = !string.IsNullOrEmpty(t.Language) ? t.Language : "und";
                string ch = t.ChannelCount > 0 ? $"{t.ChannelCount}ch" : "?";
                string lbl = !string.IsNullOrEmpty(t.Label) ? t.Label : $"[{lang}] {ch}";
                if (t.IsDualMono) lbl += " (dual-mono)";
                labels.Add(lbl);
            }
            _audioTrackDropdown.AssignEntries(labels);
            int sel = _activePlayer.SelectedAudioTrackIndex;
            if (sel >= 0 && sel < labels.Count) _audioTrackDropdown.SetValueWithoutNotify(labels[sel]);
            _audioTrackDropdown.gameObject.SetActive(tracks.Count > 0);
        }

        private void SetGroupsActive(bool active)
        {
            _controlGroup?.SetActive(active && HasControlPermission());
            _userGroup?.SetActive(active);

            bool showAdmin = active && IsAdmin() && _activeNetworking != null;
            if (_adminGroup != null && _adminGroup.gameObject.activeSelf != showAdmin)
            {
                _adminGroup.gameObject.SetActive(showAdmin);
                _adminGroup.ForceRebuild();
                _panel?.Descriptor?.ForceRebuild();
            }
        }

        private void BuildAdminGroup(RectTransform parent)
        {
            _adminGroup = PanelElementDescriptor.CreateNew(PanelElementDescriptor.ElementStyles.Group, parent);
            _adminGroup.SetTitle("Admin");
            _adminGroup.SetDescription("Network-synced policy. Visible only to clients with the basis.mediaplayer.control or * permission.");
            RectTransform content = _adminGroup.ContentParent;

            _adminOnlyToggle = PanelToggle.CreateNewEntry(content);
            _adminOnlyToggle.Descriptor.SetTitle("Admin Only");
            _adminOnlyToggle.Descriptor.SetDescription("Only clients with the control permission can take ownership.");
            _adminOnlyToggle.SetValueWithoutNotify(false);
            _adminOnlyToggle.OnValueChanged = v =>
            {
                if (_activeNetworking == null)
                {
                    return;
                }

                _ = _activeNetworking.SetAdminOnly(v);
            };

            _allowAnyoneToggle = PanelToggle.CreateNewEntry(content);
            _allowAnyoneToggle.Descriptor.SetTitle("Allow Anyone To Take Control");
            _allowAnyoneToggle.Descriptor.SetDescription("When Admin Only is off, allows non-owners to take ownership.");
            _allowAnyoneToggle.SetValueWithoutNotify(true);
            _allowAnyoneToggle.OnValueChanged = v =>
            {
                if (_activeNetworking == null)
                {
                    return;
                }

                _ = _activeNetworking.SetAllowAnyoneToTakeControl(v);
            };

            _driftSlider = PanelSlider.CreateNew(content);
            _driftSlider.SetSliderSettings(PanelSlider.SliderSettings.Advanced("Drift Seek Threshold (s)", 0f, 10f, false, 2, ValueDisplayMode.Raw));
            _driftSlider.SetValueWithoutNotify(2f);
            _driftSlider.OnValueChanged = v =>
            {
                if (_activeNetworking == null)
                {
                    return;
                }

                _ = _activeNetworking.SetDriftSeekThresholdSeconds(v);
            };

            // Deactivate AFTER children are built. PanelElementDescriptor.Awake calls
            // SetTitle(DefaultTitle)/SetDescription(DefaultDescription); if a child is
            // instantiated under an inactive parent its Awake fires later and would
            // overwrite the labels set above.
            _adminGroup.gameObject.SetActive(false);
        }

        private void BuildStatusGroup(RectTransform parent)
        {
            _statusGroup = PanelElementDescriptor.CreateNew(PanelElementDescriptor.ElementStyles.Group, parent);
            _statusGroup.SetTitle("Status");
            _statusGroup.SetDescription("—");
        }

        private void BuildDebugGroup(RectTransform parent)
        {
            _debugGroup = PanelElementDescriptor.CreateNew(PanelElementDescriptor.ElementStyles.Group, parent);
            _debugGroup.SetTitle("Debug");
            _debugGroup.SetDescription("Toggle to surface live pipeline counters.");
            RectTransform content = _debugGroup.ContentParent;

            _debugToggle = PanelToggle.CreateNewEntry(content);
            _debugToggle.Descriptor.SetTitle("Debug Mode");
            _debugToggle.SetValueWithoutNotify(false);
            _debugToggle.OnValueChanged = v =>
            {
                _debugMode = v;
                ApplyVerboseLoggingToActivePlayer(v);
                if (v) RefreshDebugInfo();
                else _debugGroup?.SetDescription("Toggle to surface live pipeline counters.");
            };

            // See BuildAdminGroup: deactivate after children so their Awake doesn't
            // clobber the title above.
            _debugGroup.gameObject.SetActive(false);
        }

        private void ApplyAdvancedVisibility(bool visible)
        {
            if (_debugGroup != null)
            {
                _debugGroup.gameObject.SetActive(visible);
                _debugGroup.ForceRebuild();
            }
            _controlGroup?.ForceRebuild();
        }

        private void ApplyCaptionOptionsVisibility(bool visible)
        {
            _captionTextOpacitySlider?.gameObject.SetActive(visible);
            _captionBgOpacitySlider?.gameObject.SetActive(visible);
            _userGroup?.ForceRebuild();
        }

        private void ApplyVerboseLoggingToActivePlayer(bool enabled)
        {
            if (_activePlayer != null) _activePlayer.VerboseLogging = enabled;
        }

        private void SetPanelTickSubscription(bool subscribe)
        {
            if (subscribe == _panelTickSubscribed) return;
            if (subscribe)
            {
                BasisFrameClock.AddRequest();
                BasisFrameClock.OnTick += OnPanelTick;
            }
            else
            {
                BasisFrameClock.OnTick -= OnPanelTick;
                BasisFrameClock.RemoveRequest();
            }
            _panelTickSubscribed = subscribe;
        }

        private void OnPanelTick()
        {
            RefreshStatus();
            if (_debugMode) RefreshDebugInfo();
        }

        // Builds the always-visible status line for the selected player: a colored
        // state word, a resolution detail, and the error/issue text when present.
        // Markup is code-assembled (trusted) but any player-supplied text (error
        // messages) is wrapped in <noparse> so its characters aren't read as tags.
        private void RefreshStatus()
        {
            if (_statusGroup == null || _activePlayer == null) return;

            BasisMediaPlayerStatus status = _activePlayer.Status;
            string err = _activePlayer.LastErrorMessage;
            Vector2Int size = _activePlayer.VideoSize;

            // Cheap gate: rebuild the markup only when something observable changed, so
            // a steady-state video doesn't allocate a string every frame. LastErrorMessage
            // returns a stable reference between changes, so ReferenceEquals is enough.
            if (status == _lastStatus && size == _lastStatusSize && ReferenceEquals(err, _lastStatusErr)) return;
            _lastStatus = status;
            _lastStatusSize = size;
            _lastStatusErr = err;

            _statusBuilder.Clear();
            _statusBuilder.Append("<color=").Append(StatusColorHex(status)).Append("><b>")
                .Append(StatusLabel(status)).Append("</b></color>");

            if (status == BasisMediaPlayerStatus.Error)
            {
                if (!string.IsNullOrEmpty(err))
                    _statusBuilder.Append("\n<color=#E5534B><noparse>").Append(err).Append("</noparse></color>");
            }
            else
            {
                if (size.x > 0 && size.y > 0)
                    _statusBuilder.Append("\n<color=#9AA0A6>").Append(size.x).Append(" x ").Append(size.y).Append("</color>");

                // A non-fatal issue (e.g. audio muted on a sample-rate mismatch):
                // video still plays, so the state word stays accurate and this is
                // surfaced as a separate amber note.
                if (!string.IsNullOrEmpty(err))
                    _statusBuilder.Append("\n<color=#E6C15A>Issue: <noparse>").Append(err).Append("</noparse></color>");
            }

            string markup = _statusBuilder.ToString();
            if (string.Equals(_lastStatusMarkup, markup)) return;
            _lastStatusMarkup = markup;
            _statusGroup.SetRichDescription(markup);
        }

        private static string StatusLabel(BasisMediaPlayerStatus status)
        {
            switch (status)
            {
                case BasisMediaPlayerStatus.NoMedia: return "No media loaded";
                case BasisMediaPlayerStatus.Connecting: return "Connecting";
                case BasisMediaPlayerStatus.Buffering: return "Buffering";
                case BasisMediaPlayerStatus.Ready: return "Ready";
                case BasisMediaPlayerStatus.Playing: return "Playing";
                case BasisMediaPlayerStatus.Paused: return "Paused";
                case BasisMediaPlayerStatus.Stopped: return "Stopped";
                case BasisMediaPlayerStatus.Ended: return "Ended";
                case BasisMediaPlayerStatus.Error: return "Error";
                default: return status.ToString();
            }
        }

        private static string StatusColorHex(BasisMediaPlayerStatus status)
        {
            switch (status)
            {
                case BasisMediaPlayerStatus.Playing: return "#57C77A"; // green
                case BasisMediaPlayerStatus.Ready: return "#5AA9E6";   // blue
                case BasisMediaPlayerStatus.Connecting:
                case BasisMediaPlayerStatus.Buffering:
                case BasisMediaPlayerStatus.Paused: return "#E6C15A";  // amber
                case BasisMediaPlayerStatus.Error: return "#E5534B";   // red
                default: return "#9AA0A6";                             // grey
            }
        }

        private void RefreshDebugInfo()
        {
            if (_debugGroup == null) return;
            if (_activePlayer == null)
            {
                _debugGroup.SetDescription("No active player.");
                return;
            }

            _debugBuilder.Clear();
            var eng = _activePlayer.NativeEngine;
            string backend = eng != null ? "OS-codec engine" : (_activePlayer.Source != null ? _activePlayer.Source.GetType().Name : "(no source)");
            _debugBuilder.Append("Backend: ").Append(backend).Append('\n');

            string state = _activePlayer.IsPlaying ? (_activePlayer.IsPaused ? "Paused" : "Playing") : "Stopped";
            _debugBuilder.Append("State: ").Append(state).Append('\n');

            var sz = _activePlayer.VideoSize;
            _debugBuilder.Append("Size: ").Append(sz.x > 0 ? $"{sz.x} x {sz.y}" : "—").Append('\n');

            long mediaUs = _activePlayer.Clock != null ? _activePlayer.Clock.CurrentMediaTimeUs : 0;
            _debugBuilder.Append("Position: ").Append(mediaUs / 1000L).Append(" ms\n");

            _debugBuilder.Append("Queue: ").Append(_activePlayer.QueuedFrameCount).Append(" / ").Append(_activePlayer.MaxQueueLength).Append('\n');
            _debugBuilder.Append("Presented: ").Append(_activePlayer.PresentedFrameCount).Append('\n');
            _debugBuilder.Append("Dropped: ").Append(_activePlayer.DroppedFrameCount)
                .Append(" (overflow ").Append(_activePlayer.OverflowDropCount)
                .Append(", late ").Append(_activePlayer.LateSkipCount)
                .Append(", fmt ").Append(_activePlayer.FormatErrorCount).Append(")\n");

            if (eng != null)
            {
                _debugBuilder.Append("Engine: ").Append(eng.State).Append('\n');
                string dbg = eng.DebugInfo;
                if (!string.IsNullOrEmpty(dbg)) _debugBuilder.Append(dbg);
            }

            var audio = _activePlayer.AudioComponent;
            if (audio != null)
            {
                _debugBuilder.Append("\nAudio: ")
                    .Append(audio.IsAnyOutputPlaying ? "playing" : "idle")
                    .Append(" peak ").Append(audio.LastPcmPeak.ToString("F3"))
                    .Append(" rms ").Append(audio.LastPcmRms.ToString("F3"));
            }

            _debugGroup.SetDescription(_debugBuilder.ToString());
        }

        private static RectTransform BuildActionRow(RectTransform parent)
        {
            GameObject rowGO = new GameObject("MediaPlayerActions", typeof(RectTransform));
            RectTransform rowRect = (RectTransform)rowGO.transform;
            rowRect.SetParent(parent, false);

            rowRect.anchorMin = new Vector2(0f, 1f);
            rowRect.anchorMax = new Vector2(1f, 1f);
            rowRect.pivot = new Vector2(0.5f, 1f);

            HorizontalLayoutGroup hlg = rowGO.AddComponent<HorizontalLayoutGroup>();
            hlg.childForceExpandWidth = true;
            hlg.childForceExpandHeight = false;
            hlg.childControlWidth = true;
            hlg.childControlHeight = true;
            hlg.spacing = 8f;
            hlg.padding = new RectOffset(8, 8, 4, 8);

            ContentSizeFitter fitter = rowGO.AddComponent<ContentSizeFitter>();
            fitter.verticalFit = ContentSizeFitter.FitMode.PreferredSize;

            LayoutElement layout = rowGO.AddComponent<LayoutElement>();
            layout.flexibleWidth = 1f;

            return rowRect;
        }
    }
}
