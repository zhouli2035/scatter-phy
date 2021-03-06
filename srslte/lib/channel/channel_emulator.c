#include "srslte/channel/channel_emulator.h"
#include "gauss.h"

int channel_emulator_initialization(channel_emulator_t* chann_emulator) {
  // Create channel related structures.
  for(uint32_t channel_id = 0; channel_id < CH_EMULATOR_NOF_CHANNELS; channel_id++) {
    // Create named pipe.
    if(channel_emulator_create_named_pipe(channel_id) < 0) {
      CH_EMULATOR_ERROR("Error creating named pipe.\n",0);
      return -1;
    }
    // Get reading named pipe.
    chann_emulator->channels[channel_id].rd_fd = channel_emulator_get_named_pipe_for_reading(channel_id);
    if(chann_emulator->channels[channel_id].rd_fd < 0) {
      CH_EMULATOR_ERROR("Error opening channel emulator writer.\n",0);
      return -1;
    }
    // Get writing named pipe.
    chann_emulator->channels[channel_id].wr_fd = channel_emulator_get_named_pipe_for_writing(channel_id);
    if(chann_emulator->channels[channel_id].wr_fd < 0) {
      CH_EMULATOR_ERROR("Error opening channel emulator writer.\n",0);
      return -1;
    }
    // Initialize counters.
    chann_emulator->channels[channel_id].nof_reads = 0;
  }
  // Initialize subframe length.
  chann_emulator->subframe_length = DEFAULT_SUBFRAME_LEN;
  // Callback used to enable liquid-dsp channel impairments.
  chann_emulator->set_channel_impairments_func_ptr = &channel_emulator_set_channel_impairments;
  // Callback used to enable simple AWGN channel.
  chann_emulator->set_channel_simple_awgn_func_ptr = &channel_emulator_set_simple_awgn_channel;
  // Initialize flag for channel impairments generated by liquid-dsp library.
  chann_emulator->enable_channel_impairments = false;
  // Initialize flag for enabling use of simple AWGN channel from srsLTE library.
  chann_emulator->use_simple_awgn_channel = false;
  // Set simple AWGN channel variance to 0.
  chann_emulator->channel_impairments.noise_variance = 0.0;
  // Set simple SNR channel variance to 100.
  chann_emulator->channel_impairments.snr = 100.0;
  // Create channel impairments.
  chann_emulator->channel_impairments.impairments = channel_cccf_create();
  // Set Channel emulator's Callbacks.
  chann_emulator->channel_impairments.add_awgn_func_ptr = &channel_emulator_add_awgn;
  chann_emulator->channel_impairments.add_simple_awgn_func_ptr = &channel_emulator_add_simple_awgn_snr;
  chann_emulator->channel_impairments.add_carrier_offset_func_ptr = &channel_emulator_add_carrier_offset;
  chann_emulator->channel_impairments.add_multipath_func_ptr = &channel_emulator_add_multipath;
  chann_emulator->channel_impairments.add_shadowing_func_ptr = &channel_emulator_add_shadowing;
  chann_emulator->channel_impairments.print_channel_func_ptr = &channel_emulator_print_channel;
  chann_emulator->channel_impairments.estimate_psd_func_ptr = &channel_emulator_estimate_psd;
  chann_emulator->channel_impairments.create_psd_script_func_ptr = &channel_emulator_create_psd_script;
  chann_emulator->channel_impairments.set_cfo_freq_func_ptr = &channel_emulator_set_cfo_freq;
  // Create CFO generator.
  if(srslte_cfo_init_finer(&chann_emulator->channel_impairments.cfocorr, DEFAULT_SUBFRAME_LEN)) {
    fprintf(stderr, "Error initiating CFO\n");
    return -1;
  }
  // Set the number of FFT bins used.
  chann_emulator->channel_impairments.fft_size = srslte_symbol_sz(DEFAULT_NOF_PRB);
  srslte_cfo_set_fft_size_finer(&chann_emulator->channel_impairments.cfocorr, chann_emulator->channel_impairments.fft_size);
  // Set default value for CFO frequency.
  chann_emulator->channel_impairments.cfo_freq = 0.0;
  // Allocate and initialize memory for random transmission delay.
#if(ENABLE_WRITING_RANDOM_ZEROS_SUFFIX==1 || ENABLE_WRITING_RANDOM_ZEROS_PREFIX==1)
  uint32_t nof_subframes = 6;
  chann_emulator->null_sample_vector = (cf_t*)srslte_vec_malloc(nof_subframes*DEFAULT_SUBFRAME_LEN*sizeof(cf_t));
  // Check if memory allocation was correctly done.
  if(chann_emulator->null_sample_vector == NULL) {
    CH_EMULATOR_ERROR("Error allocating memory for NULL Sample vector.\n",0);
    return -1;
  }
  bzero(chann_emulator->null_sample_vector, sizeof(cf_t)*nof_subframes*DEFAULT_SUBFRAME_LEN);
#endif
  // Instantiate communicator module so that the emulator can have its parameters changed online.
  communicator_make("MODULE_CHANNEL_EMULATOR", "MODULE_MAC", NULL, &chann_emulator->handle);
  // Start channel emulator configuration thread.
  if(channel_emulator_start_config_thread(chann_emulator) < 0) {
    CH_EMULATOR_ERROR("Error starting channel emulator configuration thread.\n",0);
    return -1;
  }
  // Everything went well.
  return 0;
}

int channel_emulator_start_config_thread(channel_emulator_t* const chann_emulator) {
  // Enable change parameters thread.
  chann_emulator->run_change_parameters_thread = true;
  // Create thread to change channel emulator parameters.
  pthread_attr_init(&chann_emulator->change_parameters_thread_attr);
  pthread_attr_setdetachstate(&chann_emulator->change_parameters_thread_attr, PTHREAD_CREATE_JOINABLE);
  // Create thread to change channel emulator parameters.
  int rc = pthread_create(&chann_emulator->change_parameters_thread_id, &chann_emulator->change_parameters_thread_attr, channel_emulator_change_parameters_work, (void *)chann_emulator);
  if(rc) {
    CH_EMULATOR_ERROR("Return code from channel emulator change parameters pthread_create() is %d\n", rc);
    return -1;
  }
  // Everyhting went well.
  return 0;
}

int channel_emulator_stop_config_thread(channel_emulator_t* const chann_emulator) {
  // Stop change parameters thread.
  chann_emulator->run_change_parameters_thread = false;
  // Destroy thread parameters.
  pthread_attr_destroy(&chann_emulator->change_parameters_thread_attr);
  int rc = pthread_join(chann_emulator->change_parameters_thread_id, NULL);
  if(rc) {
    CH_EMULATOR_ERROR("Return code from channel emulator change parameters thread pthread_join() is %d\n", rc);
    return -1;
  }
  // Everyhting went well.
  return 0;
}

// Thread used to change channel emulator parameters in real time.
void *channel_emulator_change_parameters_work(void *h) {
  channel_emulator_t* const chann_emulator = (channel_emulator_t*)h;
  bool ret;
  channel_emulator_config_t channel_emulator_config;
  float last_snr = 1000.0;

  // Main loop.
  while(chann_emulator->run_change_parameters_thread) {

    // Try to retrieve a message from the QUEUE. It waits for a specified amount of time before timing out.
    ret = communicator_get_low_queue_wait_for(chann_emulator->handle, 500, (void * const)&channel_emulator_config, NULL);
    if(ret) {
      if(channel_emulator_config.enable_simple_awgn_channel) {
        channel_emulator_set_simple_awgn_channel((void*)chann_emulator, channel_emulator_config.enable_simple_awgn_channel);
        if(channel_emulator_config.snr != last_snr) {
          channel_emulator_add_simple_awgn_snr((void*)chann_emulator, channel_emulator_config.snr);
          // Print new values.
          CH_EMULATOR_PRINT("Enable simple awgn channel: %s\n", channel_emulator_config.enable_simple_awgn_channel==true?"TRUE":"FALSE");
          CH_EMULATOR_PRINT("SNR: %1.2f [dB]\n", channel_emulator_config.snr);
          // Update last configured SNR.
          last_snr = channel_emulator_config.snr;
        }
      }
    }
  }

  CH_EMULATOR_PRINT("Leaving change channel emulator parameters thread.\n", 0);
  // Exit thread with result code.
  pthread_exit(NULL);
}

int channel_emulator_uninitialization(channel_emulator_t* chann_emulator) {
  // Stop channel emulator configuration thread.
  if(channel_emulator_stop_config_thread(chann_emulator) < 0) {
    CH_EMULATOR_ERROR("Error when stopping channel emulator configuration thread.\n", 0);
    return -1;
  }
  CH_EMULATOR_PRINT("Channel emulator configuration thread stopped successfully\n", 0);
  // Close channel related objects.
  for(uint32_t channel_id = 0; channel_id < CH_EMULATOR_NOF_CHANNELS; channel_id++) {
    // Close channel emulator writing pipe.
    if(channel_emulator_close_writing_pipe(chann_emulator->channels[channel_id].wr_fd, channel_id) < 0) {
      return -1;
    }
    // Close channel emulator reading pipe.
    if(channel_emulator_close_reading_pipe(chann_emulator->channels[channel_id].rd_fd, channel_id) < 0) {
      return -1;
    }
  }
  // Destroy channel impairments object.
  channel_cccf_destroy(chann_emulator->channel_impairments.impairments);
  // Destroy all CFO related structures.
  srslte_cfo_free_finer(&chann_emulator->channel_impairments.cfocorr);
  // Free memory used to store Application context object.
#if(ENABLE_WRITING_RANDOM_ZEROS_SUFFIX==1 || ENABLE_WRITING_RANDOM_ZEROS_PREFIX==1)
  if(chann_emulator->null_sample_vector) {
    free(chann_emulator->null_sample_vector);
    chann_emulator->null_sample_vector = NULL;
  }
#endif
  // After use, communicator handle MUST be freed.
  communicator_free(&chann_emulator->handle);
  // Everything went well.
  return 0;
}

// Add AWGN impairment generated by liquid-dsp library.
void channel_emulator_add_awgn(void *h, float noise_floor, float SNRdB) {
  channel_emulator_t *ch_emulator = (channel_emulator_t*)h;
  channel_cccf_add_awgn(ch_emulator->channel_impairments.impairments, noise_floor, SNRdB);
}

// Add AWGN impairment generated by simple AWGN channel from srsLTE library.
void channel_emulator_add_simple_awgn(void *h, float noise_variance) {
  channel_emulator_t *ch_emulator = (channel_emulator_t*)h;
  ch_emulator->channel_impairments.noise_variance = noise_variance;
}

void channel_emulator_add_simple_awgn_snr(void *h, float snr) {
  channel_emulator_t *ch_emulator = (channel_emulator_t*)h;
  ch_emulator->channel_impairments.snr = snr;
}

// Add carrier offset impairment.
void channel_emulator_add_carrier_offset(void *h, float dphi, float phi) {
  channel_emulator_t *ch_emulator = (channel_emulator_t*)h;
  channel_cccf_add_carrier_offset(ch_emulator->channel_impairments.impairments, dphi, phi);
}

// Add multipath impairment.
void channel_emulator_add_multipath(void *h, cf_t *hc, unsigned int hc_len) {
  channel_emulator_t *ch_emulator = (channel_emulator_t*)h;
  channel_cccf_add_multipath(ch_emulator->channel_impairments.impairments, hc, hc_len);
}

// Add shadowing impairment.
void channel_emulator_add_shadowing(void *h, float sigma, float fd) {
  channel_emulator_t *ch_emulator = (channel_emulator_t*)h;
  channel_cccf_add_shadowing(ch_emulator->channel_impairments.impairments, sigma, fd);
}

// Print channel impairments internals.
void channel_emulator_print_channel(void * h) {
  channel_emulator_t *ch_emulator = (channel_emulator_t*)h;
  channel_cccf_print(ch_emulator->channel_impairments.impairments);
}

// Estimate spectrum.
void channel_emulator_estimate_psd(cf_t *sig, uint32_t nof_samples, unsigned int nfft, float *psd) {
  spgramcf_estimate_psd(nfft, sig, nof_samples, psd);
}

void channel_emulator_create_psd_script(unsigned int nfft, float *psd) {
  FILE * fid = fopen(OUTPUT_FILENAME, "w");
  // power spectral density estimate
  fprintf(fid,"nfft = %u;\n", nfft);
  fprintf(fid,"f=[0:(nfft-1)]/nfft - 0.5;\n");
  fprintf(fid,"psd = zeros(1,nfft);\n");
  for(uint32_t i = 0; i < nfft; i++) {
    fprintf(fid,"psd(%3u) = %12.8f;\n", i+1, psd[i]);
  }

  fprintf(fid,"  plot(f, psd, 'LineWidth',1.5,'Color',[0 0.5 0.2]);\n");
  fprintf(fid,"  grid on;\n");
  fprintf(fid,"  pmin = 10*floor(0.1*min(psd - 5));\n");
  fprintf(fid,"  pmax = 10*ceil (0.1*max(psd + 5));\n");
  fprintf(fid,"  axis([-0.5 0.5 pmin pmax]);\n");
  fprintf(fid,"  xlabel('Normalized Frequency [f/F_s]');\n");
  fprintf(fid,"  ylabel('Power Spectral Density [dB]');\n");

  fclose(fid);
  printf("results written to %s.\n", OUTPUT_FILENAME);
}

int channel_emulator_set_subframe_length(channel_emulator_t* chann_emulator, double freq) {
  int subframe_length = -1;
  if(freq > 0) {
    subframe_length = (int)(freq*0.001);
    chann_emulator->subframe_length = subframe_length;
  }
  return subframe_length;
}

// Function used to enabled liquid-dsp channel impairments.
void channel_emulator_set_channel_impairments(void* h, bool flag) {
  channel_emulator_t* chann_emulator = (channel_emulator_t*)h;
  chann_emulator->enable_channel_impairments = flag;
  // If liquid-dsp channel impairments are being enabled then we disable simple AWGN channel.
  if(flag) {
    chann_emulator->use_simple_awgn_channel = false;
  }
}

// Function used to enabled simple srsLTE based AWGN channel.
void channel_emulator_set_simple_awgn_channel(void* h, bool flag) {
  channel_emulator_t* chann_emulator = (channel_emulator_t*)h;
  chann_emulator->use_simple_awgn_channel = flag;
  // If simple AWGN channel is being enabled then we disable simple liquid-dsp channel impairments.
  if(flag) {
    chann_emulator->enable_channel_impairments = false;
  }
}

void channel_emulator_set_cfo_freq(void* h, float freq) {
  channel_emulator_t* chann_emulator = (channel_emulator_t*)h;
  chann_emulator->channel_impairments.cfo_freq = freq;
}

int channel_emulator_create_named_pipe(uint32_t channel_id) {
  int ret = 0;
  char path[100];
  // Create named pipe name file name.
  sprintf(path,"%s_%d", CH_EMULATOR_NP_FILENAME_CH, channel_id);
  CH_EMULATOR_INFO("Creating named pipe: %s\n", path);
  // Create named pipe.
  ret = mkfifo(path, 0666);
  if(ret < 0) {
    if(errno != EEXIST) {
      char error_str[100];
      get_error_string(errno, error_str);
      CH_EMULATOR_ERROR("Error: %s\n", error_str);
      return -1;
    } else {
      CH_EMULATOR_INFO("The named pipe, %s, already exists.\n", path);
    }
  }

  return 0;
}

int channel_emulator_get_named_pipe_for_writing(uint32_t channel_id) {
  int ret = 0, fd = -1;
  char path[100];
  // Create named pipe name file name.
  sprintf(path,"%s_%d", CH_EMULATOR_NP_FILENAME_CH, channel_id);
  // Open named pipe for read/write.
  fd = open(path, O_WRONLY);
  if(fd < 0) {
    CH_EMULATOR_ERROR("Error opening writing pipe for channel %d: %d\n", channel_id, errno);
    return -1;
  }
  // Increase the named pipe size.
  ret = fcntl(fd, F_SETPIPE_SZ, DEFAULT_SUBFRAME_LEN*sizeof(cf_t));
  if(ret < 0) {
    CH_EMULATOR_ERROR("Error increasing pipe size for channel %d: %d\n", channel_id, errno);
    return -1;
  }
  CH_EMULATOR_PRINT("Named pipe for channel %d increasing to size: %d\n", channel_id, DEFAULT_SUBFRAME_LEN*sizeof(cf_t));

  return fd;
}

int channel_emulator_get_named_pipe_for_reading(uint32_t channel_id) {
  int fd = -1;
  char path[100];
  // Create named pipe name file name.
  sprintf(path,"%s_%d", CH_EMULATOR_NP_FILENAME_CH, channel_id);
  // First open in read only mode.
  fd = open(path, O_RDONLY|O_NONBLOCK);
  if(fd < 0) {
    CH_EMULATOR_ERROR("Error opening reading pipe for channel %d: %d\n", channel_id, errno);
    return -1;
  }
  return fd;
}

int channel_emulator_close_reading_pipe(int fd, uint32_t channel_id) {
  int ret = -1;
  ret = close(fd);
  if(ret < 0) {
    CH_EMULATOR_ERROR("Error closing reading pipe for channel %d: %d\n", channel_id, errno);
  }
  return ret;
}

int channel_emulator_close_writing_pipe(int fd, uint32_t channel_id) {
  int ret = -1;
  ret = close(fd);
  if(ret < 0) {
    CH_EMULATOR_ERROR("Error closing writing pipe for channel %d: %d\n", channel_id, errno);
  }
  return ret;
}

int channel_emulator_send(void* h, void *data, int nof_samples, bool blocking, bool is_start_of_burst, bool is_end_of_burst, uint32_t channel_id) {

  channel_emulator_t *ch_emulator = (channel_emulator_t*)h;
  int ret = 0;
  int32_t nof_written_samples = 0;
#if(ENABLE_WRITING_ZEROS==1)
  uint32_t additional_samples = 0;
#endif
  static float last_snr[CH_EMULATOR_NOF_CHANNELS] = {1000.0, 1000.0};

  // Calculate SNR.
  if(ch_emulator->use_simple_awgn_channel && ch_emulator->channel_impairments.snr != last_snr[channel_id] && is_start_of_burst) {
    // Calculate signal power in dBW.
    float tx_rssi = 10*log10(srslte_vec_avg_power_cf((cf_t*)data, nof_samples));
    // Calculate noise power in dBW.
    float noise_power = tx_rssi - ch_emulator->channel_impairments.snr;
    // Calculate noise variance.
    ch_emulator->channel_impairments.noise_variance = pow(10.0, noise_power/10.0);
    // Print calculated values.
    printf("[Channel Emulator] PHY ID: %d - SNR: %1.2f [dB] - Signal power: %1.2f [dBW] - Noise power: %1.2f [dBW] - Noise variance: %1.2e\n", channel_id, (tx_rssi-noise_power), tx_rssi, noise_power, ch_emulator->channel_impairments.noise_variance);
    // Update last noise variance variable.
    last_snr[channel_id] = ch_emulator->channel_impairments.snr;
  }

  // Write a random number of zeros before the subframe in order to emulate real-world transmission.
#if(ENABLE_WRITING_RANDOM_ZEROS_PREFIX==1)
  // Get random number of additional zero samples.
  if(is_start_of_burst) {
    uint32_t nof_random_samples = (rand() % (5*ch_emulator->subframe_length));
    // Sleep for the duration of the random number of samples.
#if(ADD_DELAY_BEFORE_RANDOM_SAMPLES==1)
    uint32_t random_samples_delay = (uint32_t)((((float)nof_random_samples)*1000.0)/((float)ch_emulator->subframe_length));
    usleep(8*random_samples_delay);
#endif
    // Write random number of samples.
    if(nof_random_samples > 0) {
      //printf("channel_id: %d - nof_random_samples: %d\n", channel_id, nof_random_samples);
      ret = write(ch_emulator->channels[channel_id].wr_fd, ch_emulator->null_sample_vector, nof_random_samples*sizeof(cf_t));
      if(ret < 0) {
        CH_EMULATOR_ERROR("Write to named pipe returned: %d\n", ret);
        return -1;
      } else {
        nof_written_samples = ret/sizeof(cf_t);
      }
    }
  }

  ret = write(ch_emulator->channels[channel_id].wr_fd, data, nof_samples*sizeof(cf_t));
  if(ret < 0) {
    CH_EMULATOR_ERROR("Write to named pipe returned: %d\n", ret);
    return -1;
  } else {
    nof_written_samples += ret/sizeof(cf_t);
  }

#else
  ret = write(ch_emulator->channels[channel_id].wr_fd, data, nof_samples*sizeof(cf_t));
  if(ret < 0) {
    CH_EMULATOR_ERROR("Write to named pipe returned: %d\n", ret);
    return -1;
  } else {
    nof_written_samples = ret/sizeof(cf_t);
  }
#endif

  // Write a random number of zeros to emulate real-world transmission.
#if(ENABLE_WRITING_RANDOM_ZEROS_SUFFIX==1)
  if(ret > 0) {
    // Get random number of additional zero samples.
    uint32_t nof_random_samples = rand() % ch_emulator->subframe_length;
    ret = write(ch_emulator->channels[channel_id].wr_fd, ch_emulator->null_sample_vector, nof_random_samples*sizeof(cf_t));
    if(ret < 0) {
      CH_EMULATOR_ERROR("Write to named pipe returned: %d\n", ret);
    } else {
      nof_written_samples += ret/sizeof(cf_t);
    }
  }
#endif

#if(ENABLE_WRITING_ZEROS==1)
  // If number of samples is not a integer multiple of subframe_length then write zeros so that we have a multiple of that.
  if(nof_samples > ch_emulator->subframe_length && ret > 0) {
    // Calculate number of additional zero samples.
    additional_samples = ch_emulator->subframe_length - (nof_samples % ch_emulator->subframe_length);
    for(uint32_t i = 0; i < additional_samples; i++) {
      ret = write(ch_emulator->channels[channel_id].wr_fd, ch_emulator->null_sample_vector, sizeof(cf_t));
      if(ret < 0) {
        CH_EMULATOR_ERROR("Write to named pipe returned: %d\n", ret);
        break;
      } else {
        nof_written_samples++;
      }
    }
  }
#endif

  return nof_written_samples;
}

int channel_emulator_recv(void *h, void *data, uint32_t nof_samples, bool blocking, uint32_t channel_id) {

  channel_emulator_t *ch_emulator = (channel_emulator_t*)h;

  // Receive data.
  int ret = recv_samples(h, data, nof_samples, channel_id);

  //if(ret > 0) CH_EMULATOR_PRINT("Read %d samples\n", ret);

  //if(ret > 0 && ret > ch_emulator->subframe_length) CH_EMULATOR_PRINT("Read %d samples...........................\n", ret);

  // Only apply further processing if we received samples.
  if(ret > 0) {
    // Apply simple AWGN channel to the received signal. It has precedence over the liquid-dsp library channel.
    if(ch_emulator->use_simple_awgn_channel) {
      srslte_ch_awgn_c((cf_t*)data, (cf_t*)data, sqrt(ch_emulator->channel_impairments.noise_variance), nof_samples);
    } else if(ch_emulator->enable_channel_impairments) {
      // Apply channel impairments from liquid-dsp library to the input signal if enabled.
      channel_cccf_execute_block(ch_emulator->channel_impairments.impairments, data, nof_samples, data);
    }
    // Apply CFO to the signal.
    if(ch_emulator->channel_impairments.cfo_freq > 0.0) {
      srslte_cfo_correct_finer(&ch_emulator->channel_impairments.cfocorr, data, data, ch_emulator->channel_impairments.cfo_freq/((float)ch_emulator->channel_impairments.fft_size));
      CH_EMULATOR_INFO("Applying CFO of %f [Hz] to the subframe.\n", ch_emulator->channel_impairments.cfo_freq*15000.0);
    }
  }

  return ret;
}

int recv_samples(void *h, void *data, uint32_t nof_samples, uint32_t channel_id) {
  channel_emulator_t *ch_emulator = (channel_emulator_t*)h;
  int ret;
  cf_t *samples = (cf_t*)data;
  // Read specified number of samples from named pipe.
  do {
    ret = read(ch_emulator->channels[channel_id].rd_fd, (void*)&samples[ch_emulator->channels[channel_id].nof_reads], (nof_samples-ch_emulator->channels[channel_id].nof_reads)*sizeof(cf_t));
    if(ret > 0) {
      ch_emulator->channels[channel_id].nof_reads += ret/sizeof(cf_t);
    } else {
      if(ret < 0) {
        return ret;
      }
    }
  } while(ch_emulator->channels[channel_id].nof_reads < nof_samples && ret != 0); // If zero is returned, then the writing side of the named fifo was closed.
  nof_samples = ch_emulator->channels[channel_id].nof_reads;
  // Reset number of reads counter.
  ch_emulator->channels[channel_id].nof_reads = 0;

  return nof_samples;
}
