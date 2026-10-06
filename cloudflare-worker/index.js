export default {
  async fetch(request, env, ctx) {
    // Chỉ chấp nhận POST request
    if (request.method !== 'POST') {
      return new Response('Method not allowed', { status: 405 });
    }

    try {
      // 1. Nhận luồng Audio từ ESP32 (WAV)
      // ESP32 sẽ gửi lên dưới dạng file raw hoặc Blob
      const audioBlob = await request.blob();
      
      // 2. STT: Gọi Groq Whisper
      const whisperFormData = new FormData();
      whisperFormData.append('file', audioBlob, 'audio.wav');
      whisperFormData.append('model', 'whisper-large-v3'); // Hoặc whisper-large-v3-turbo
      whisperFormData.append('language', 'vi');

      const whisperRes = await fetch('https://api.groq.com/openai/v1/audio/transcriptions', {
        method: 'POST',
        headers: { 'Authorization': `Bearer ${env.GROQ_API_KEY}` },
        body: whisperFormData
      });
      const whisperData = await whisperRes.json();
      if (!whisperData.text) {
        throw new Error("Không nhận diện được giọng nói.");
      }
      const userText = whisperData.text;
      console.log("User Text:", userText);

      // 3. LLM: Gọi Groq Llama 3
      const llamaRes = await fetch('https://api.groq.com/openai/v1/chat/completions', {
        method: 'POST',
        headers: {
          'Authorization': `Bearer ${env.GROQ_API_KEY}`,
          'Content-Type': 'application/json'
        },
        body: JSON.stringify({
          model: 'llama-3.3-70b-versatile', // Thay bằng model Llama mới nhất trên Groq
          messages: [
            { role: 'system', content: 'Bạn là trợ lý ảo tiếng Việt thông minh, dễ thương. Trả lời ngắn gọn dưới 3 câu.' },
            { role: 'user', content: userText }
          ]
        })
      });
      const llamaData = await llamaRes.json();
      const aiText = llamaData.choices[0].message.content;
      console.log("AI Text:", aiText);

      // 4. TTS: Gọi FPT.AI Voice (Hoặc Google TTS)
      // TẠM THỜI DÙNG GOOGLE TTS ĐỂ BẠN TEST TRƯỚC (Vì nó miễn phí & không cần config dài dòng)
      // Khi lấy được tài liệu API chuẩn từ trang FPT mới, ta sẽ thay URL vào đây
      
      const ttsUrl = `https://translate.google.com/translate_tts?ie=UTF-8&q=${encodeURIComponent(aiText)}&tl=vi&client=tw-ob`;
      const ttsRes = await fetch(ttsUrl);
      
      // Mẫu code FPT.AI (Cần thay đổi URL chuẩn theo trang API Reference của FPT Token Factory)
      /*
      const fptRes = await fetch('https://api.fpt.ai/hmi/tts/v5', {
        method: 'POST',
        headers: {
          'api-key': env.FPT_API_KEY,
          'voice': 'banmai',
          'speed': '', 'format': 'mp3'
        },
        body: aiText
      });
      const fptData = await fptRes.json();
      const ttsRes = await fetch(fptData.async); // Fetch link mp3 trả về
      */
      
      // 5. Trả thẳng luồng MP3 về cho ESP32
      return new Response(ttsRes.body, {
        headers: { 
          'Content-Type': 'audio/mpeg',
          'X-User-Text': encodeURIComponent(userText), // Gửi kèm text để ESP32 hiển thị ra màn hình
          'X-AI-Text': encodeURIComponent(aiText)
        }
      });

    } catch (err) {
      return new Response(err.message, { status: 500 });
    }
  },
};
