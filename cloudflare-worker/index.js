export default {
  async fetch(request, env, ctx) {
    if (request.method !== 'POST') {
      return new Response('Method not allowed', { status: 405 });
    }

    try {
      const audioBlob = await request.blob();
      
      // 1. STT: Gọi Groq Whisper
      const whisperFormData = new FormData();
      whisperFormData.append('file', audioBlob, 'audio.wav');
      whisperFormData.append('model', 'whisper-large-v3'); 
      whisperFormData.append('language', 'vi');

      const whisperRes = await fetch('https://api.groq.com/openai/v1/audio/transcriptions', {
        method: 'POST',
        headers: { 'Authorization': `Bearer ${env.GROQ_API_KEY}` },
        body: whisperFormData
      });
      const whisperData = await whisperRes.json();
      if (!whisperData.text) throw new Error("Không nhận diện được giọng nói.");
      const userText = whisperData.text;

      // 2. LLM: Gọi Groq Llama 3
      const llamaRes = await fetch('https://api.groq.com/openai/v1/chat/completions', {
        method: 'POST',
        headers: {
          'Authorization': `Bearer ${env.GROQ_API_KEY}`,
          'Content-Type': 'application/json'
        },
        body: JSON.stringify({
          model: 'llama-3.3-70b-versatile',
          messages: [
            { role: 'system', content: 'Bạn là trợ lý ảo tiếng Việt thông minh. Trả lời ngắn gọn dưới 3 câu.' },
            { role: 'user', content: userText }
          ]
        })
      });
      const llamaData = await llamaRes.json();
      const aiText = llamaData.choices[0].message.content;

      // 3. TTS: Gọi FPT.AI Voice (V5)
      const fptRes = await fetch('https://api.fpt.ai/hmi/tts/v5', {
        method: 'POST',
        headers: { 
          'api-key': env.FPT_API_KEY, 
          'voice': 'banmai', 
          'speed': '', 
          'format': 'mp3' 
        },
        body: aiText
      });
      const fptData = await fptRes.json();
      
      // Nếu API trả về lỗi hoặc không có link async
      if (!fptData.async) {
        throw new Error("Lỗi API FPT: " + JSON.stringify(fptData));
      }
      const ttsUrl = fptData.async;

      // 4. Trả về JSON để ESP32 dễ dàng hiển thị chữ và phát nhạc
      return new Response(JSON.stringify({
        userText: userText,
        aiText: aiText,
        ttsUrl: ttsUrl
      }), {
        headers: { 'Content-Type': 'application/json' }
      });

    } catch (err) {
      return new Response(JSON.stringify({ error: err.message }), { status: 500 });
    }
  },
};
